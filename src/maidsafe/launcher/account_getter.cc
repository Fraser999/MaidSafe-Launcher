/*  Copyright 2014 MaidSafe.net limited

    This MaidSafe Software is licensed to you under (1) the MaidSafe.net Commercial License,
    version 1.0 or later, or (2) The General Public License (GPL), version 3, depending on which
    licence you accepted on initial access to the Software (the "Licences").

    By contributing code to the MaidSafe Software, or to this project generally, you agree to be
    bound by the terms of the MaidSafe Contributor Agreement, version 1.0, found in the root
    directory of this project at LICENSE, COPYING and CONTRIBUTOR respectively and also
    available at: http://www.maidsafe.net/licenses

    Unless required by applicable law or agreed to in writing, the MaidSafe Software distributed
    under the GPL Licence is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS
    OF ANY KIND, either express or implied.

    See the Licences for the specific language governing permissions and limitations relating to
    use of the MaidSafe Software.                                                                 */

#include "maidsafe/launcher/account_getter.h"

#include <string>

#include "maidsafe/common/make_unique.h"

#include "maidsafe/launcher/launcher.h"

namespace maidsafe {

namespace launcher {

std::future<std::unique_ptr<AccountGetter>> AccountGetter::CreateAccountGetter() {
  return std::async(std::launch::async,
                    [] { return std::unique_ptr<AccountGetter>(new AccountGetter); });
}

AccountGetter::AccountGetter()
    : network_health_mutex_(),
      network_health_condition_variable_(),
      network_health_(-1),
#ifdef ROUTING_AND_NFS_UPDATED
#ifdef USE_FAKE_STORE
      data_getter_(maidsafe::make_unique<DataGetter>(Launcher::FakeStorePath(),
                                                     Launcher::FakeStoreDiskUsage())),
#else
      routing_(maidsafe::make_unique<routing::Routing>()),
      data_getter_(),  // deferred construction until asio service is created
#endif
#else
      data_getter_(maidsafe::make_unique<DataGetter>(
          MemoryUsage(1 << 7), Launcher::FakeStoreDiskUsage(), nullptr, Launcher::FakeStorePath())),
#endif
      //      public_pmid_helper_(),
      asio_service_(2) {
#ifndef USE_FAKE_STORE
  data_getter_ = maidsafe::make_unique<DataGetter>(asio_service_, *routing_);
  InitRouting();
  static_cast<void>(data_getter);
#endif
}

AccountGetter::~AccountGetter() {
#ifndef USE_FAKE_STORE
  data_getter_->Stop();
  routing_.reset();
#endif
}

#ifndef USE_FAKE_STORE

void AccountGetter::InitRouting() {
  routing::Functors functors(InitialiseRoutingCallbacks());
  routing_->Join(functors);
  // FIXME BEFORE_RELEASE discuss: parallel attempts, max no. of endpoints to try,
  // prioritise live ports. To reduce the blocking duration in case of no network connectivity
  std::unique_lock<std::mutex> lock{network_health_mutex_};
  network_health_condition_variable_.wait(
      lock, [this] { return (network_health_ == 100) || (network_health_ < -300000); });
  if (network_health_ < 0)
    BOOST_THROW_EXCEPTION(MakeError(RoutingErrors::not_connected));
}

routing::Functors AccountGetter::InitialiseRoutingCallbacks() {
  routing::Functors functors;
  functors.typed_message_and_caching.group_to_single.message_received = [this](
      const routing::GroupToSingleMessage& message) { data_getter_->HandleMessage(message); };
  // Copying routing node id as routing object on destruction fires network_status functor.
  // As we take it in account getter's asio thread, it tries to re-enter routing after destruction
  auto this_node_id = routing_->kNodeId();
  functors.network_status = [this, this_node_id](const int& network_health) {
    OnNetworkStatusChange(network_health, this_node_id);
  };
  functors.close_nodes_change =
      [this](std::shared_ptr<routing::CloseNodesChange> /*close_nodes_change*/) {};
  functors.request_public_key =
      [this](const NodeId& node_id, const routing::GivePublicKeyFunctor& give_key) {
        auto future_key(data_getter_->Get(passport::PublicPmid::Name{Identity{node_id.string()}},
                                          std::chrono::seconds(10)));
        public_pmid_helper_.AddEntry(std::move(future_key), give_key);
      };

  // Required to pick cached messages
  functors.typed_message_and_caching.single_to_single.message_received = [this](
      const routing::SingleToSingleMessage& message) { data_getter_->HandleMessage(message); };

  // TODO(Prakash) fix routing asserts for clients so private_client need not to provide callbacks
  // for all functors
  functors.typed_message_and_caching.single_to_group.message_received =
      [this](const routing::SingleToGroupMessage& /*message*/) {};
  functors.typed_message_and_caching.group_to_group.message_received =
      [this](const routing::GroupToGroupMessage& /*message*/) {};
  functors.typed_message_and_caching.single_to_group_relay.message_received =
      [this](const routing::SingleToGroupRelayMessage& /*message*/) {};
  functors.typed_message_and_caching.single_to_group.put_cache_data =
      [this](const routing::SingleToGroupMessage& /*message*/) {};
  functors.typed_message_and_caching.group_to_single.put_cache_data =
      [this](const routing::GroupToSingleMessage& /*message*/) {};
  functors.typed_message_and_caching.group_to_group.put_cache_data =
      [this](const routing::GroupToGroupMessage& /*message*/) {};

  return functors;
}

void AccountGetter::OnNetworkStatusChange(int updated_network_health, const NodeId& node_id) {
  asio_service_.service().post([=] {
    routing::UpdateNetworkHealth(updated_network_health, network_health_, network_health_mutex_,
                                 network_health_condition_variable_, node_id);
  });
}

#endif

}  // namespace launcher

}  // namespace maidsafe
