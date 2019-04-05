/*
 * Copyright (c) 2015, 2018, Oracle and/or its affiliates. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2.0,
 * as published by the Free Software Foundation.
 *
 * This program is also distributed with certain software (including
 * but not limited to OpenSSL) that is licensed under separate terms,
 * as designated in a particular file or component or in included license
 * documentation.  The authors of MySQL hereby grant you an additional
 * permission to link the program and your derivative works with the
 * separately licensed software that they have included with MySQL.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License, version 2.0, for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
 */

#include <algorithm>
#include <new>

#include "my_inttypes.h"
#include "my_io.h"
#include "violite.h"

#include "plugin/x/ngs/include/ngs/interface/connection_acceptor_interface.h"
#include "plugin/x/ngs/include/ngs/socket_events.h"
#include "plugin/x/ngs/include/ngs_common/operations_factory.h"

// Surpressing numerous warnings generated by libevent on Windows.
#ifdef WIN32
#pragma warning(push)
#pragma warning(disable : 4005)
#endif              // WIN32
#include <event.h>  // libevent
#ifdef WIN32
#pragma warning(pop)
#endif  // WIN32

namespace ngs {

class Connection_acceptor_socket : public Connection_acceptor_interface {
 public:
  typedef Socket_interface::Shared_ptr Socket_ptr;

  Connection_acceptor_socket(Socket_ptr listener,
                             System_interface &system_interface)
      : m_socket_listener(listener), m_system_interface(system_interface) {}

  Vio *accept() {
    Vio *vio;
    sockaddr_storage accept_address;
    MYSQL_SOCKET sock = MYSQL_INVALID_SOCKET;

    for (int i = 0; i < MAX_ACCEPT_REATTEMPT; ++i) {
      socklen_t accept_len = sizeof(accept_address);

      sock = m_socket_listener->accept(KEY_socket_x_client_connection,
                                       (struct sockaddr *)&accept_address,
                                       &accept_len);

      if (INVALID_SOCKET != mysql_socket_getfd(sock)) break;

      const int error_code = m_system_interface.get_socket_errno();
      if (error_code != SOCKET_EINTR && error_code != SOCKET_EAGAIN)
        return NULL;
    }

    const bool is_tcpip = (accept_address.ss_family == AF_INET ||
                           accept_address.ss_family == AF_INET6);
    vio = mysql_socket_vio_new(sock,
                               is_tcpip ? VIO_TYPE_TCPIP : VIO_TYPE_SOCKET, 0);
    if (!vio) throw std::bad_alloc();

    // enable TCP_NODELAY
    vio_fastsend(vio);
    vio_keepalive(vio, true);

    return vio;
  }

 private:
  Socket_ptr m_socket_listener;
  System_interface &m_system_interface;
  static const int MAX_ACCEPT_REATTEMPT = 10;
};

struct Socket_events::Timer_data {
  ngs::function<bool()> callback;
  event ev;
  timeval tv;
  Socket_events *self;

  static void free(Timer_data *data) {
    evtimer_del(&data->ev);
    ngs::free_object(data);
  }
};

struct Socket_events::Socket_data {
  ngs::function<void(Connection_acceptor_interface &)> callback;
  event ev;
  Socket_interface::Shared_ptr socket;

  static void free(Socket_data *data) {
    event_del(&data->ev);
    ngs::free_object(data);
  }
};

Socket_events::Socket_events() {
  m_evbase = event_base_new();

  if (!m_evbase) throw std::bad_alloc();
}

Socket_events::~Socket_events() {
  std::for_each(m_timer_events.begin(), m_timer_events.end(),
                &Timer_data::free);

  std::for_each(m_socket_events.begin(), m_socket_events.end(),
                &Socket_data::free);

  event_base_free(m_evbase);
}

bool Socket_events::listen(
    Socket_interface::Shared_ptr sock,
    ngs::function<void(Connection_acceptor_interface &)> callback) {
  m_socket_events.push_back(ngs::allocate_object<Socket_data>());
  Socket_data *socket_event = m_socket_events.back();

  socket_event->callback = callback;
  socket_event->socket = sock;

  event_set(&socket_event->ev, static_cast<int>(sock->get_socket_fd()),
            EV_READ | EV_PERSIST, &Socket_events::socket_data_avaiable,
            socket_event);
  event_base_set(m_evbase, &socket_event->ev);

  return 0 == event_add(&socket_event->ev, NULL);
}

/** Register a callback to be executed in a fixed time interval.

The callback is called from the server's event loop thread, until either
the server is stopped or the callback returns false.

NOTE: This method may only be called from the same thread as the event loop.
*/
void Socket_events::add_timer(const std::size_t delay_ms,
                              ngs::function<bool()> callback) {
  Timer_data *data = ngs::allocate_object<Timer_data>();
  data->tv.tv_sec = static_cast<long>(delay_ms / 1000);
  data->tv.tv_usec = (delay_ms % 1000) * 1000;
  data->callback = callback;
  data->self = this;
  // XXX use persistent timer events after switch to libevent2
  evtimer_set(&data->ev, timeout_call, data);
  event_base_set(m_evbase, &data->ev);
  evtimer_add(&data->ev, &data->tv);

  MUTEX_LOCK(lock, m_timers_mutex);
  m_timer_events.push_back(data);
}

void Socket_events::loop() { event_base_loop(m_evbase, 0); }

void Socket_events::break_loop() { event_base_loopbreak(m_evbase); }

void Socket_events::timeout_call(socket_type, short, void *arg) {
  Timer_data *data = (Timer_data *)arg;

  if (!data->callback()) {
    evtimer_del(&data->ev);

    {
      MUTEX_LOCK(timer_lock, data->self->m_timers_mutex);
      data->self->m_timer_events.erase(
          std::remove(data->self->m_timer_events.begin(),
                      data->self->m_timer_events.end(), data),
          data->self->m_timer_events.end());
    }

    ngs::free_object(data);
  } else {
    // schedule for another round
    evtimer_add(&data->ev, &data->tv);
  }
}

void Socket_events::socket_data_avaiable(socket_type, short, void *arg) {
  Socket_data *data = (Socket_data *)arg;
  Operations_factory operations_factory;
  System_interface::Shared_ptr system_interface(
      operations_factory.create_system_interface());
  Connection_acceptor_socket acceptor(data->socket, *system_interface);

  data->callback(acceptor);
}

}  // namespace ngs
