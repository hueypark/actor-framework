/******************************************************************************
 *                       ____    _    _____                                   *
 *                      / ___|  / \  |  ___|    C++                           *
 *                     | |     / _ \ | |_       Actor                         *
 *                     | |___ / ___ \|  _|      Framework                     *
 *                      \____/_/   \_|_|                                      *
 *                                                                            *
 * Copyright 2011-2018 Dominik Charousset                                     *
 *                                                                            *
 * Distributed under the terms and conditions of the BSD 3-Clause License or  *
 * (at your option) under the terms and conditions of the Boost Software      *
 * License 1.0. See accompanying files LICENSE and LICENSE_ALTERNATIVE.       *
 *                                                                            *
 * If you did not receive a copy of the license files, see                    *
 * http://opensource.org/licenses/BSD-3-Clause and                            *
 * http://www.boost.org/LICENSE_1_0.txt.                                      *
 ******************************************************************************/

#include "caf/uniform_type_info_map.hpp"

#include <ios> // std::ios_base::failure
#include <array>
#include <tuple>
#include <limits>
#include <string>
#include <vector>
#include <cstring> // memcmp
#include <algorithm>
#include <type_traits>

#include "caf/abstract_group.hpp"
#include "caf/actor_cast.hpp"
#include "caf/actor_factory.hpp"
#include "caf/actor_system.hpp"
#include "caf/actor_system_config.hpp"
#include "caf/downstream_msg.hpp"
#include "caf/group.hpp"
#include "caf/locks.hpp"
#include "caf/logger.hpp"
#include "caf/message.hpp"
#include "caf/message_builder.hpp"
#include "caf/proxy_registry.hpp"
#include "caf/string_algorithms.hpp"
#include "caf/timespan.hpp"
#include "caf/timestamp.hpp"
#include "caf/type_nr.hpp"
#include "caf/upstream_msg.hpp"

#include "caf/detail/safe_equal.hpp"
#include "caf/detail/scope_guard.hpp"
#include "caf/detail/shared_spinlock.hpp"

namespace caf {

const char* numbered_type_names[] = {
  "@actor",
  "@actorvec",
  "@addr",
  "@addrvec",
  "@bytebuf",
  "@charbuf",
  "@config_value",
  "@down",
  "@downstream_msg",
  "@error",
  "@exit",
  "@group",
  "@group_down",
  "@i16",
  "@i32",
  "@i64",
  "@i8",
  "@ldouble",
  "@message",
  "@message_id",
  "@node",
  "@open_stream_msg",
  "@str",
  "@strmap",
  "@strong_actor_ptr",
  "@strset",
  "@strvec",
  "@timeout",
  "@timespan",
  "@timestamp",
  "@u16",
  "@u16_str",
  "@u32",
  "@u32_str",
  "@u64",
  "@u8",
  "@unit",
  "@upstream_msg",
  "@weak_actor_ptr",
  "bool",
  "caf::add_atom",
  "caf::close_atom",
  "caf::connect_atom",
  "caf::contact_atom",
  "caf::delete_atom",
  "caf::demonitor_atom",
  "caf::div_atom",
  "caf::flush_atom",
  "caf::forward_atom",
  "caf::get_atom",
  "caf::idle_atom",
  "caf::join_atom",
  "caf::leave_atom",
  "caf::link_atom",
  "caf::migrate_atom",
  "caf::monitor_atom",
  "caf::mul_atom",
  "caf::ok_atom",
  "caf::open_atom",
  "caf::pending_atom",
  "caf::ping_atom",
  "caf::pong_atom",
  "caf::publish_atom",
  "caf::publish_udp_atom",
  "caf::put_atom",
  "caf::receive_atom",
  "caf::redirect_atom",
  "caf::resolve_atom",
  "caf::spawn_atom",
  "caf::stream_atom",
  "caf::sub_atom",
  "caf::subscribe_atom",
  "caf::sys_atom",
  "caf::tick_atom",
  "caf::unlink_atom",
  "caf::unpublish_atom",
  "caf::unpublish_udp_atom",
  "caf::unsubscribe_atom",
  "caf::update_atom",
  "caf::wait_for_atom",
  "double",
  "float",
};

namespace {

using builtins = std::array<uniform_type_info_map::value_factory_kvp,
                            type_nrs - 1>;

void fill_builtins(builtins&, detail::type_list<>, size_t) {
  // end of recursion
}

template <class List>
void fill_builtins(builtins& arr, List, size_t pos) {
  using type = typename detail::tl_head<List>::type;
  typename detail::tl_tail<List>::type next;
  arr[pos].first = numbered_type_names[pos];
  arr[pos].second = &make_type_erased_value<type>;
  fill_builtins(arr, next, pos + 1);
}

} // namespace

type_erased_value_ptr uniform_type_info_map::make_value(uint16_t nr) const {
  return builtin_[nr - 1].second();
}

type_erased_value_ptr
uniform_type_info_map::make_value(const std::string& x) const {
  auto pred = [&](const value_factory_kvp& kvp) {
    return kvp.first == x;
  };
  auto e = builtin_.end();
  auto i = std::find_if(builtin_.begin(), e, pred);
  if (i != e)
    return i->second();
  auto& custom_names = system().config().value_factories_by_name;
  auto j = custom_names.find(x);
  if (j != custom_names.end())
    return j->second();
  return nullptr;
}

type_erased_value_ptr
uniform_type_info_map::make_value(const std::type_info& x) const {
  auto& custom_by_rtti = system().config().value_factories_by_rtti;
  auto i = custom_by_rtti.find(std::type_index(x));
  if (i != custom_by_rtti.end())
    return i->second();
  return nullptr;
}

const std::string&
uniform_type_info_map::portable_name(uint16_t nr,
                                     const std::type_info* ti) const {
  if (nr != 0)
    return builtin_names_[nr - 1];
  if (ti == nullptr)
    return default_type_name_;
  auto& custom_names = system().config().type_names_by_rtti;
  auto i = custom_names.find(std::type_index(*ti));
  if (i != custom_names.end())
    return i->second;
  return default_type_name_;
}

uniform_type_info_map::uniform_type_info_map(actor_system& sys)
  : system_(sys), default_type_name_("???") {
  sorted_builtin_types list;
  fill_builtins(builtin_, list, 0);
  for (size_t i = 0; i < builtin_names_.size(); ++i)
    builtin_names_[i] = numbered_type_names[i];
}

} // namespace caf
