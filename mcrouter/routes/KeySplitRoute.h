/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the LICENSE
 * file in the root directory of this source tree.
 */
#pragma once

#include <folly/Optional.h>
#include <folly/fibers/AddTasks.h>
#include <folly/fibers/FiberManager.h>

#include "mcrouter/lib/McKey.h"
#include "mcrouter/lib/McResUtil.h"
#include "mcrouter/lib/Operation.h"
#include "mcrouter/lib/RouteHandleTraverser.h"
#include "mcrouter/lib/fbi/cpp/FuncGenerator.h"
#include "mcrouter/lib/fbi/cpp/globals.h"
#include "mcrouter/lib/network/gen/MemcacheMessages.h"
#include "mcrouter/lib/network/gen/MemcacheRouteHandleIf.h"

namespace facebook {
namespace memcache {

template <class RouteHandleIf>
class RouteHandleFactory;

namespace mcrouter {

/**
 * This route handle will allow a particular key to live on more than one
 * host in a destination pool. This is to primarily mitigate hot keys
 * overwhelming a single host.
 *
 * This is done by rehashing the key with a value and routing based on the
 * new key.
 */
class KeySplitRoute {
 public:
  std::string routeName() const {
    uint64_t replicaId = getReplicaId();
    return folly::sformat(
        "keysplit|replicas={}|all-sync={}|replicaId={}",
        replicas_,
        allSync_,
        replicaId);
  }

  static constexpr size_t kMinReplicaCount = 2;
  static constexpr size_t kMaxReplicaCount = 1000;

  template <class Request>
  void traverse(
      const Request& req,
      const RouteHandleTraverser<MemcacheRouteHandleIf>& t) const {
    uint64_t replicaId = getReplicaId();
    if (shouldAugmentRequest(replicaId)) {
      t(*child_, copyAndAugment(req, replicaId));
    } else {
      t(*child_, req);
    }
  }

  KeySplitRoute(
      std::shared_ptr<MemcacheRouteHandleIf> child,
      size_t replicas,
      bool allSync)
      : child_(std::move(child)), replicas_(replicas), allSync_(allSync) {
    assert(child_ != nullptr);
    assert(replicas_ >= kMinReplicaCount);
    assert(replicas_ <= kMaxReplicaCount);
  }

  // Route only to 1 replica.
  template <class Request>
  typename std::enable_if<
      folly::
          IsOneOf<Request, McGetRequest, McLeaseGetRequest, McLeaseSetRequest>::
              value,
      ReplyT<Request>>::type
  route(const Request& req) const {
    if (!canAugmentRequest(req)) {
      return child_->route(req);
    }

    // always retrieve from 1 replica
    uint64_t replicaId = getReplicaId();
    return routeOne(req, replicaId);
  }

  // Route only if all sync is enabled. Otherwise, route normally
  template <class Request>
  typename std::enable_if<
      folly::IsOneOf<Request, McSetRequest>::value,
      ReplyT<Request>>::type
  route(const Request& req) const {
    if (!canAugmentRequest(req)) {
      return child_->route(req);
    }

    uint64_t replicaId = getReplicaId();

    // Only set to all key replicas if we have it enabled
    if (allSync_) {
      return routeAll(req, replicaId);
    }
    return routeOne(req, replicaId);
  }

  // Unconditionally route to all replicas.
  template <class Request>
  typename std::enable_if<
      folly::IsOneOf<Request, McDeleteRequest>::value,
      ReplyT<Request>>::type
  route(const Request& req) const {
    if (!canAugmentRequest(req)) {
      return child_->route(req);
    }

    // always delete all key replicas
    uint64_t replicaId = getReplicaId();
    return routeAll(req, replicaId);
  }

  // fallback to just normal routing through one of the replicas
  template <class Request>
  typename std::enable_if<
      !folly::IsOneOf<
          Request,
          McGetRequest,
          McLeaseGetRequest,
          McLeaseSetRequest,
          McSetRequest,
          McDeleteRequest>::value,
      ReplyT<Request>>::type
  route(const Request& req) const {
    uint64_t replicaId = getReplicaId();
    return routeOne(req, replicaId);
  }

 private:
  static constexpr folly::StringPiece kMemcacheReplicaSeparator = "::";
  static constexpr size_t kMaxMcKeyLength = 255;
  static constexpr size_t kExtraKeySpaceNeeded =
      kMemcacheReplicaSeparator.size() +
      detail::numDigitsBase10(kMaxReplicaCount - 1);

  // route configuration
  const std::shared_ptr<MemcacheRouteHandleIf> child_;
  const size_t replicas_{2};
  const bool allSync_{false};

  template <class Request>
  bool canAugmentRequest(const Request& req) const {
    // don't augment if length of key is too long
    return req.key().fullKey().size() + kExtraKeySpaceNeeded <= kMaxMcKeyLength;
  }

  bool shouldAugmentRequest(size_t replicaId) const {
    // first replica will route normally without key change.
    return replicaId > 0;
  }

  template <class Request>
  Request copyAndAugment(Request& originalReq, uint64_t replicaId) const {
    auto req = originalReq;
    req.key() = folly::to<std::string>(
        req.key().fullKey(), kMemcacheReplicaSeparator, replicaId);
    return req;
  }

  template <class Request>
  ReplyT<Request> routeOne(const Request& req, uint64_t replicaId) const {
    if (shouldAugmentRequest(replicaId)) {
      return child_->route(copyAndAugment(req, replicaId));
    } else {
      return child_->route(req);
    }
  }

  template <class Request>
  ReplyT<Request> routeAll(const Request& req, uint64_t replicaId) const {
    // send off to all replicas async except the one we are assigned to
    for (size_t id = 0; id < replicas_; ++id) {
      if (id == replicaId) {
        continue;
      }

      // we need to make a copy to send to the fiber
      auto reqCopy = shouldAugmentRequest(id) ? copyAndAugment(req, id) : req;
      folly::fibers::addTask(
          [child = child_, reqReplica = std::move(reqCopy)]() {
            return child->route(reqReplica);
          });
    }

    return routeOne(req, replicaId);
  }

  uint64_t getReplicaId() const {
    return globals::hostid() % replicas_;
  }
};

std::shared_ptr<MemcacheRouteHandleIf> makeKeySplitRoute(
    RouteHandleFactory<MemcacheRouteHandleIf>& factory,
    const folly::dynamic& json);

} // namespace mcrouter
} // namespace memcache
} // namespace facebook