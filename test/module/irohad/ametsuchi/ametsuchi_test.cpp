/**
 * Copyright Soramitsu Co., Ltd. 2017 All Rights Reserved.
 * http://soramitsu.co.jp
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>
#include <boost/range/algorithm/for_each.hpp>
#include <boost/range/combine.hpp>
#include "module/shared_model/builders/protobuf/test_block_builder.hpp"
#include "module/shared_model/builders/protobuf/test_transaction_builder.hpp"

#include "ametsuchi/impl/postgres_block_query.hpp"
#include "ametsuchi/impl/postgres_ordering_service_persistent_state.hpp"
#include "ametsuchi/impl/postgres_wsv_query.hpp"
#include "ametsuchi/impl/storage_impl.hpp"
#include "ametsuchi/mutable_storage.hpp"
#include "common/byteutils.hpp"
#include "framework/test_subscriber.hpp"
#include "model/account.hpp"
#include "model/account_asset.hpp"
#include "model/asset.hpp"
#include "model/commands/all.hpp"
#include "model/converters/pb_block_factory.hpp"
#include "model/domain.hpp"
#include "model/peer.hpp"
#include "model/permissions.hpp"
#include "model/sha3_hash.hpp"
#include "module/irohad/ametsuchi/ametsuchi_fixture.hpp"

using namespace iroha::ametsuchi;
using namespace iroha::model;
using namespace framework::test_subscriber;

auto zero_string = std::string("0", 32);
auto fake_hash = shared_model::crypto::Hash(zero_string);
auto fake_pubkey = shared_model::crypto::PublicKey(zero_string);

/**
 * Shortcut to create CallExact observable wrapper, subscribe with given lambda,
 * and validate the number of calls with optional custom output
 * @tparam O observable type
 * @tparam F on_next function type
 * @param o observable object
 * @param f function object
 * @param call_count number of expected calls
 * @param msg custom validation failure message
 */
template <typename O, typename F>
void validateCalls(O &&o,
                   F &&f,
                   uint64_t call_count,
                   const std::string &msg = {}) {
  auto wrap = make_test_subscriber<CallExact>(std::forward<O>(o), call_count);
  wrap.subscribe(std::forward<F>(f));
  ASSERT_TRUE(wrap.validate()) << "Expected " << call_count << " calls" << msg;
}

/**
 * Validate getAccountTransaction with given parameters
 * @tparam B block query type
 * @param blocks block query object
 * @param account id to query
 * @param call_count number of observable calls
 * @param command_count number of commands in transaction
 */
template <typename B>
void validateAccountTransactions(B &&blocks,
                                 const std::string &account,
                                 int call_count,
                                 int command_count) {
  validateCalls(
      blocks->getAccountTransactions(account),
      [&](const auto &tx) { EXPECT_EQ(tx->commands().size(), command_count); },
      call_count,
      " for " + account);
}

/**
 * Validate getAccountAssetTransactions with given parameters
 * @tparam B block query type
 * @param blocks block query object
 * @param account id to query
 * @param asset id to query
 * @param call_count number of observable calls
 * @param command_count number of commands in transaction
 */
template <typename B>
void validateAccountAssetTransactions(B &&blocks,
                                      const std::string &account,
                                      const std::string &asset,
                                      int call_count,
                                      int command_count) {
  validateCalls(
      blocks->getAccountAssetTransactions(account, asset),
      [&](const auto &tx) { EXPECT_EQ(tx->commands().size(), command_count); },
      call_count,
      " for " + account + " " + asset);
}

/**
 * Validate getAccountAsset with given parameters
 * @tparam W WSV query type
 * @param wsv WSV query object
 * @param account id to query
 * @param asset id to query
 * @param amount to validate
 */
template <typename W>
void validateAccountAsset(W &&wsv,
                          const std::string &account,
                          const std::string &asset,
                          const iroha::Amount &amount) {
  auto account_asset = wsv->getAccountAsset(account, asset);
  ASSERT_TRUE(account_asset);
  ASSERT_EQ((*account_asset)->accountId(), account);
  ASSERT_EQ((*account_asset)->assetId(), asset);
  ASSERT_EQ(*((*account_asset)->balance().makeOldModel()), amount);
}

/**
 * Validate getAccount with given parameters
 * @tparam W WSV query type
 * @param wsv WSV query object
 * @param id account to query
 * @param domain id to validate
 */
template <typename W>
void validateAccount(W &&wsv,
                     const std::string &id,
                     const std::string &domain) {
  auto account = wsv->getAccount(id);
  ASSERT_TRUE(account);
  ASSERT_EQ((*account)->accountId(), id);
  ASSERT_EQ((*account)->domainId(), domain);
}

/**
 * Apply block to given storage
 * @tparam S storage type
 * @param storage storage object
 * @param block to apply
 */
template <typename S>
void apply(S &&storage, const shared_model::interface::Block &block) {
  std::unique_ptr<MutableStorage> ms;
  auto storageResult = storage->createMutableStorage();
  storageResult.match(
      [&](iroha::expected::Value<std::unique_ptr<MutableStorage>> &_storage) {
        ms = std::move(_storage.value);
      },
      [](iroha::expected::Error<std::string> &error) {
        FAIL() << "MutableStorage: " << error.error;
      });
  ms->apply(*std::unique_ptr<iroha::model::Block>(block.makeOldModel()),
            [](const auto &, auto &, const auto &) { return true; });
  storage->commit(std::move(ms));
}

TEST_F(AmetsuchiTest, GetBlocksCompletedWhenCalled) {
  // Commit block => get block => observable completed
  std::shared_ptr<StorageImpl> storage;
  auto storageResult = StorageImpl::create(block_store_path, pgopt_);
  storageResult.match(
      [&](iroha::expected::Value<std::shared_ptr<StorageImpl>> &_storage) {
        storage = _storage.value;
      },
      [](iroha::expected::Error<std::string> &error) {
        FAIL() << "StorageImpl: " << error.error;
      });
  ASSERT_TRUE(storage);
  auto blocks = storage->getBlockQuery();

  auto block = TestBlockBuilder().height(1).prevHash(fake_hash).build();

  apply(storage, block);

  auto completed_wrapper =
      make_test_subscriber<IsCompleted>(blocks->getBlocks(1, 1));
  completed_wrapper.subscribe();
  ASSERT_TRUE(completed_wrapper.validate());
}

TEST_F(AmetsuchiTest, SampleTest) {
  std::shared_ptr<StorageImpl> storage;
  auto storageResult = StorageImpl::create(block_store_path, pgopt_);
  storageResult.match(
      [&](iroha::expected::Value<std::shared_ptr<StorageImpl>> &_storage) {
        storage = _storage.value;
      },
      [](iroha::expected::Error<std::string> &error) {
        FAIL() << "StorageImpl: " << error.error;
      });
  ASSERT_TRUE(storage);
  auto wsv = storage->getWsvQuery();
  auto blocks = storage->getBlockQuery();

  const auto domain = "ru", user1name = "user1", user2name = "user2",
             user1id = "user1@ru", user2id = "user2@ru", assetname = "RUB",
             assetid = "RUB#ru";

  std::string account, src_account, dest_account, asset;
  iroha::Amount amount;

  // Block 1
  auto block1 =
      TestBlockBuilder()
          .transactions(std::vector<shared_model::proto::Transaction>(
              {TestTransactionBuilder()
                   .creatorAccountId("admin1")
                   .createRole(
                       "user",
                       std::set<std::string>{
                           can_add_peer, can_create_asset, can_get_my_account})
                   .createDomain(domain, "user")
                   .createAccount(user1name, domain, fake_pubkey)
                   .build()}))
          .height(1)
          .prevHash(fake_hash)
          .txNumber(1)
          .build();

  apply(storage, block1);

  validateAccount(wsv, user1id, domain);

  // Block 2
  auto block2 =
      TestBlockBuilder()
          .transactions(std::vector<shared_model::proto::Transaction>(
              {TestTransactionBuilder()
                   .creatorAccountId("admin2")
                   .createAccount(user2name, domain, fake_pubkey)
                   .createAsset(assetname, domain, 1)
                   .addAssetQuantity(user1id, assetid, "150.0")
                   .transferAsset(
                       user1id, user2id, assetid, "Transfer asset", "100.0")
                   .build()}))
          .height(2)
          .prevHash(block1.hash())
          .txNumber(1)
          .build();

  apply(storage, block2);
  validateAccountAsset(
      wsv, user1id, assetid, *iroha::Amount::createFromString("50.0"));
  validateAccountAsset(
      wsv, user2id, assetid, *iroha::Amount::createFromString("100.0"));

  // Block store tests
  auto hashes = {block1.hash(), block2.hash()};
  validateCalls(blocks->getBlocks(1, 2),
                [i = 0, &hashes](auto eachBlock) mutable {
                  EXPECT_EQ(*(hashes.begin() + i), eachBlock->hash());
                  ++i;
                },
                2);

  validateAccountTransactions(blocks, "admin1", 1, 3);
  validateAccountTransactions(blocks, "admin2", 1, 4);
  validateAccountTransactions(blocks, "non_existing_user", 0, 0);

  validateAccountAssetTransactions(blocks, user1id, assetid, 1, 4);
  validateAccountAssetTransactions(blocks, user2id, assetid, 1, 4);
  validateAccountAssetTransactions(
      blocks, "non_existing_user", "non_existing_asset", 0, 0);
}

TEST_F(AmetsuchiTest, PeerTest) {
  std::shared_ptr<StorageImpl> storage;
  auto storageResult = StorageImpl::create(block_store_path, pgopt_);
  storageResult.match(
      [&](iroha::expected::Value<std::shared_ptr<StorageImpl>> &_storage) {
        storage = _storage.value;
      },
      [](iroha::expected::Error<std::string> &error) {
        FAIL() << "StorageImpl: " << error.error;
      });
  ASSERT_TRUE(storage);
  auto wsv = storage->getWsvQuery();

  auto txn = TestTransactionBuilder()
                 .addPeer("192.168.9.1:50051", fake_pubkey)
                 .build();

  auto block =
      TestBlockBuilder()
          .txNumber(1)
          .transactions(std::vector<shared_model::proto::Transaction>{txn})
          .prevHash(fake_hash)
          .build();

  apply(storage, block);

  auto peers = wsv->getPeers();
  ASSERT_TRUE(peers);
  ASSERT_EQ(peers->size(), 1);
  ASSERT_EQ(*(peers->at(0)->makeOldModel()), addPeer.peer);
}

TEST_F(AmetsuchiTest, queryGetAccountAssetTransactionsTest) {
  std::shared_ptr<StorageImpl> storage;
  auto storageResult = StorageImpl::create(block_store_path, pgopt_);
  storageResult.match(
      [&](iroha::expected::Value<std::shared_ptr<StorageImpl>> &_storage) {
        storage = _storage.value;
      },
      [](iroha::expected::Error<std::string> &error) {
        FAIL() << "StorageImpl: " << error.error;
      });
  ASSERT_TRUE(storage);
  auto wsv = storage->getWsvQuery();
  auto blocks = storage->getBlockQuery();

  const auto admin = "admin1", domain = "domain", user1name = "user1",
             user2name = "user2", user3name = "user3", user1id = "user1@domain",
             user2id = "user2@domain", user3id = "user3@domain",
             asset1name = "asset1", asset2name = "asset2",
             asset1id = "asset1#domain", asset2id = "asset2#domain";

  std::string account, src_account, dest_account, asset;
  iroha::Amount amount;

  // 1st tx
  auto txn1 =
      TestTransactionBuilder()
          .creatorAccountId(admin)
          .createRole("user",
                      std::set<std::string>{
                          can_add_peer, can_create_asset, can_get_my_account})
          .createDomain(domain, "user")
          .createAccount(user1name, domain, fake_pubkey)
          .createAccount(user2name, domain, fake_pubkey)
          .createAccount(user3name, domain, fake_pubkey)
          .createAsset(asset1name, domain, 1)
          .createAsset(asset2name, domain, 1)
          .addAssetQuantity(user1id, asset1id, "300.0")
          .addAssetQuantity(user2id, asset2id, "250.0")
          .build();

  auto block1 =
      TestBlockBuilder()
          .height(1)
          .transactions(std::vector<shared_model::proto::Transaction>({txn1}))
          .prevHash(fake_hash)
          .txNumber(1)
          .build();

  apply(storage, block1);

  // Check querying accounts
  for (const auto &id : {user1id, user2id, user3id}) {
    validateAccount(wsv, id, domain);
  }

  // Check querying assets for users
  validateAccountAsset(
      wsv, user1id, asset1id, *iroha::Amount::createFromString("300.0"));
  validateAccountAsset(
      wsv, user2id, asset2id, *iroha::Amount::createFromString("250.0"));

  // 2th tx (user1 -> user2 # asset1)
  auto txn2 =
      TestTransactionBuilder()
          .creatorAccountId(user1id)
          .transferAsset(user1id, user2id, asset1id, "Transfer asset", "120.0")
          .build();
  auto block2 =
      TestBlockBuilder()
          .transactions(std::vector<shared_model::proto::Transaction>({txn2}))
          .height(2)
          .prevHash(block1.hash())
          .txNumber(1)
          .build();

  apply(storage, block2);

  // Check account asset after transfer assets
  validateAccountAsset(
      wsv, user1id, asset1id, *iroha::Amount::createFromString("180.0"));
  validateAccountAsset(
      wsv, user2id, asset1id, *iroha::Amount::createFromString("120.0"));

  // 3rd tx
  //   (user2 -> user3 # asset2)
  //   (user2 -> user1 # asset2)
  auto txn3 =
      TestTransactionBuilder()
          .creatorAccountId(user2id)
          .transferAsset(user2id, user3id, asset2id, "Transfer asset", "150.0")
          .transferAsset(user2id, user1id, asset2id, "Transfer asset", "10.0")
          .build();

  auto block3 =
      TestBlockBuilder()
          .transactions(std::vector<shared_model::proto::Transaction>({txn3}))
          .height(3)
          .prevHash(block2.hash())
          .txNumber(1)
          .build();

  apply(storage, block3);

  validateAccountAsset(
      wsv, user2id, asset2id, *iroha::Amount::createFromString("90.0"));
  validateAccountAsset(
      wsv, user3id, asset2id, *iroha::Amount::createFromString("150.0"));
  validateAccountAsset(
      wsv, user1id, asset2id, *iroha::Amount::createFromString("10.0"));

  // Block store test
  auto hashes = {block1.hash(), block2.hash(), block3.hash()};
  validateCalls(blocks->getBlocks(1, 3),
                [i = 0, &hashes](auto eachBlock) mutable {
                  EXPECT_EQ(*(hashes.begin() + i), eachBlock->hash());
                  ++i;
                },
                3);

  validateAccountTransactions(blocks, admin, 1, 9);
  validateAccountTransactions(blocks, user1id, 1, 1);
  validateAccountTransactions(blocks, user2id, 1, 2);
  validateAccountTransactions(blocks, user3id, 0, 0);

  // (user1 -> user2 # asset1)
  // (user2 -> user3 # asset2)
  // (user2 -> user1 # asset2)
  validateAccountAssetTransactions(blocks, user1id, asset1id, 1, 1);
  validateAccountAssetTransactions(blocks, user2id, asset1id, 1, 1);
  validateAccountAssetTransactions(blocks, user3id, asset1id, 0, 0);
  validateAccountAssetTransactions(blocks, user1id, asset2id, 1, 2);
  validateAccountAssetTransactions(blocks, user2id, asset2id, 1, 2);
  validateAccountAssetTransactions(blocks, user3id, asset2id, 1, 2);
}

TEST_F(AmetsuchiTest, AddSignatoryTest) {
  std::shared_ptr<StorageImpl> storage;
  auto storageResult = StorageImpl::create(block_store_path, pgopt_);
  storageResult.match(
      [&](iroha::expected::Value<std::shared_ptr<StorageImpl>> &_storage) {
        storage = _storage.value;
      },
      [](iroha::expected::Error<std::string> &error) {
        FAIL() << "StorageImpl: " << error.error;
      });
  ASSERT_TRUE(storage);
  auto wsv = storage->getWsvQuery();

  shared_model::crypto::PublicKey pubkey1(std::string("1", 32));
  shared_model::crypto::PublicKey pubkey2(std::string("2", 32));

  auto user1id = "user1@domain";
  auto user2id = "user2@domain";

  // 1st tx (create user1 with pubkey1)
  auto txn1 =
      TestTransactionBuilder()
          .creatorAccountId("admin1")
          .createRole("user",
                      std::set<std::string>{
                          can_add_peer, can_create_asset, can_get_my_account})
          .createDomain("domain", "user")
          .createAccount("user1", "domain", pubkey1)
          .build();
  auto block1 =
      TestBlockBuilder()
          .transactions(std::vector<shared_model::proto::Transaction>({txn1}))
          .height(1)
          .prevHash(fake_hash)
          .txNumber(1)
          .build();

  apply(storage, block1);

  {
    auto account_opt = wsv->getAccount(user1id);
    ASSERT_TRUE(account_opt);
    auto account = account_opt.value();
    ASSERT_EQ(account->accountId(), user1id);
    ASSERT_EQ(account->domainId(), createAccount.domain_id);

    auto signatories = wsv->getSignatories(user1id);
    ASSERT_TRUE(signatories);
    ASSERT_EQ(signatories->size(), 1);
    ASSERT_EQ(signatories->at(0),
              iroha::pubkey_t::from_string(toBinaryString(pubkey1)));
  }

  // 2nd tx (add sig2 to user1)
  auto txn2 = TestTransactionBuilder()
                  .creatorAccountId(user1id)
                  .addSignatory(user1id, pubkey2)
                  .build();

  auto block2 =
      TestBlockBuilder()
          .transactions(std::vector<shared_model::proto::Transaction>({txn2}))
          .height(2)
          .prevHash(block1.hash())
          .txNumber(1)
          .build();

  apply(storage, block2);

  {
    auto account = wsv->getAccount(user1id);
    ASSERT_TRUE(account);

    auto signatories = wsv->getSignatories(user1id);
    ASSERT_TRUE(signatories);
    ASSERT_EQ(signatories->size(), 2);
    ASSERT_EQ(signatories->at(0),
              iroha::pubkey_t::from_string(toBinaryString(pubkey1)));
    ASSERT_EQ(signatories->at(1),
              iroha::pubkey_t::from_string(toBinaryString(pubkey2)));
  }

  // 3rd tx (create user2 with pubkey1 that is same as user1's key)
  auto txn3 = TestTransactionBuilder()
                  .creatorAccountId("admin2")
                  .createAccount("user2", "domain", pubkey1)
                  .build();

  auto block3 =
      TestBlockBuilder()
          .transactions(std::vector<shared_model::proto::Transaction>({txn3}))
          .height(3)
          .prevHash(block2.hash())
          .txNumber(1)
          .build();

  apply(storage, block3);

  {
    auto account1 = wsv->getAccount(user1id);
    ASSERT_TRUE(account1);

    auto account2 = wsv->getAccount(user2id);
    ASSERT_TRUE(account2);

    auto signatories1 = wsv->getSignatories(user1id);
    ASSERT_TRUE(signatories1);
    ASSERT_EQ(signatories1->size(), 2);
    ASSERT_EQ(signatories1->at(0),
              iroha::pubkey_t::from_string(toBinaryString(pubkey1)));
    ASSERT_EQ(signatories1->at(1),
              iroha::pubkey_t::from_string(toBinaryString(pubkey2)));

    auto signatories2 = wsv->getSignatories(user2id);
    ASSERT_TRUE(signatories2);
    ASSERT_EQ(signatories2->size(), 1);
    ASSERT_EQ(signatories2->at(0),
              iroha::pubkey_t::from_string(toBinaryString(pubkey1)));
  }

  // 4th tx (remove pubkey1 from user1)
  auto txn4 = TestTransactionBuilder()
                  .creatorAccountId(user1id)
                  .removeSignatory(user1id, pubkey1)
                  .build();

  auto block4 =
      TestBlockBuilder()
          .transactions(std::vector<shared_model::proto::Transaction>({txn4}))
          .height(4)
          .prevHash(block3.hash())
          .txNumber(1)
          .build();

  apply(storage, block4);

  {
    auto account = wsv->getAccount(user1id);
    ASSERT_TRUE(account);

    // user1 has only pubkey2.
    auto signatories1 = wsv->getSignatories(user1id);
    ASSERT_TRUE(signatories1);
    ASSERT_EQ(signatories1->size(), 1);
    ASSERT_EQ(signatories1->at(0),
              iroha::pubkey_t::from_string(toBinaryString(pubkey2)));

    // user2 still has pubkey1.
    auto signatories2 = wsv->getSignatories(user2id);
    ASSERT_TRUE(signatories2);
    ASSERT_EQ(signatories2->size(), 1);
    ASSERT_EQ(signatories2->at(0),
              iroha::pubkey_t::from_string(toBinaryString(pubkey1)));
  }

  // 5th tx (add sig2 to user2 and set quorum = 1)
  auto txn5 = TestTransactionBuilder()
                  .creatorAccountId(user1id)
                  .addSignatory(user2id, pubkey2)
                  .setAccountQuorum(user2id, 2)
                  .build();

  auto block5 =
      TestBlockBuilder()
          .transactions(std::vector<shared_model::proto::Transaction>({txn5}))
          .height(5)
          .prevHash(block4.hash())
          .txNumber(1)
          .build();

  apply(storage, block5);

  {
    auto account_opt = wsv->getAccount(user2id);
    ASSERT_TRUE(account_opt);
    auto &account = account_opt.value();
    ASSERT_EQ(account->quorum(), 2);

    // user2 has pubkey1 and pubkey2.
    auto signatories = wsv->getSignatories(user2id);
    ASSERT_TRUE(signatories);
    ASSERT_EQ(signatories->size(), 2);
    ASSERT_EQ(signatories->at(0),
              iroha::pubkey_t::from_string(toBinaryString(pubkey1)));
    ASSERT_EQ(signatories->at(1),
              iroha::pubkey_t::from_string(toBinaryString(pubkey2)));
  }

  // 6th tx (remove sig2 fro user2: This must success)
  auto txn6 = TestTransactionBuilder()
                  .creatorAccountId(user2id)
                  .removeSignatory(user2id, pubkey2)
                  .setAccountQuorum(user2id, 2)
                  .build();

  auto block6 =
      TestBlockBuilder()
          .transactions(std::vector<shared_model::proto::Transaction>({txn6}))
          .height(6)
          .prevHash(block5.hash())
          .txNumber(1)
          .build();

  apply(storage, block6);

  {
    // user2 only has pubkey1.
    auto signatories = wsv->getSignatories(user2id);
    ASSERT_TRUE(signatories);
    ASSERT_EQ(signatories->size(), 1);
    ASSERT_EQ(signatories->at(0),
              iroha::pubkey_t::from_string(toBinaryString(pubkey1)));
  }
}

shared_model::proto::Block getBlock() {
  auto txn = TestTransactionBuilder()
                 .creatorAccountId("admin1")
                 .addPeer("192.168.0.0", fake_pubkey)
                 .build();

  auto block =
      TestBlockBuilder()
          .transactions(std::vector<shared_model::proto::Transaction>({txn}))
          .height(1)
          .prevHash(fake_hash)
          .txNumber(1)
          .build();
  return block;
}

TEST_F(AmetsuchiTest, TestingStorageWhenInsertBlock) {
  auto log = logger::testLog("TestStorage");
  log->info(
      "Test case: create storage "
      "=> insert block "
      "=> assert that inserted");
  std::shared_ptr<StorageImpl> storage;
  auto storageResult = StorageImpl::create(block_store_path, pgopt_);
  storageResult.match(
      [&](iroha::expected::Value<std::shared_ptr<StorageImpl>> &_storage) {
        storage = _storage.value;
      },
      [](iroha::expected::Error<std::string> &error) {
        FAIL() << "StorageImpl: " << error.error;
      });
  ASSERT_TRUE(storage);
  auto wsv = storage->getWsvQuery();
  ASSERT_EQ(0, wsv->getPeers().value().size());

  log->info("Try insert block");

  auto inserted = storage->insertBlock(
      *std::unique_ptr<iroha::model::Block>(getBlock().makeOldModel()));
  ASSERT_TRUE(inserted);

  log->info("Request ledger information");

  ASSERT_NE(0, wsv->getPeers().value().size());

  log->info("Drop ledger");

  storage->dropStorage();
}

TEST_F(AmetsuchiTest, TestingStorageWhenDropAll) {
  auto logger = logger::testLog("TestStorage");
  logger->info(
      "Test case: create storage "
      "=> insert block "
      "=> assert that written"
      " => drop all "
      "=> assert that all deleted ");

  auto log = logger::testLog("TestStorage");
  log->info(
      "Test case: create storage "
      "=> insert block "
      "=> assert that inserted");
  std::shared_ptr<StorageImpl> storage;
  auto storageResult = StorageImpl::create(block_store_path, pgopt_);
  storageResult.match(
      [&](iroha::expected::Value<std::shared_ptr<StorageImpl>> &_storage) {
        storage = _storage.value;
      },
      [](iroha::expected::Error<std::string> &error) {
        FAIL() << "StorageImpl: " << error.error;
      });
  ASSERT_TRUE(storage);
  auto wsv = storage->getWsvQuery();
  ASSERT_EQ(0, wsv->getPeers().value().size());

  log->info("Try insert block");

  auto inserted = storage->insertBlock(
      *std::unique_ptr<iroha::model::Block>(getBlock().makeOldModel()));
  ASSERT_TRUE(inserted);

  log->info("Request ledger information");

  ASSERT_NE(0, wsv->getPeers().value().size());

  log->info("Drop ledger");

  storage->dropStorage();

  ASSERT_EQ(0, wsv->getPeers().value().size());
  std::shared_ptr<StorageImpl> new_storage;
  auto new_storageResult = StorageImpl::create(block_store_path, pgopt_);
  storageResult.match(
      [&](iroha::expected::Value<std::shared_ptr<StorageImpl>> &_storage) {
        new_storage = _storage.value;
      },
      [](iroha::expected::Error<std::string> &error) {
        FAIL() << "StorageImpl: " << error.error;
      });
  ASSERT_EQ(0, wsv->getPeers().value().size());
  new_storage->dropStorage();
}

/**
 * @given initialized storage
 * @when insert block with 2 transactions in
 * @then both of them are found with getTxByHashSync call by hash. Transaction
 * with some other hash is not found.
 */
TEST_F(AmetsuchiTest, FindTxByHashTest) {
  std::shared_ptr<StorageImpl> storage;
  auto storageResult = StorageImpl::create(block_store_path, pgopt_);
  storageResult.match(
      [&](iroha::expected::Value<std::shared_ptr<StorageImpl>> &_storage) {
        storage = _storage.value;
      },
      [](iroha::expected::Error<std::string> &error) {
        FAIL() << "StorageImpl: " << error.error;
      });
  ASSERT_TRUE(storage);
  auto blocks = storage->getBlockQuery();

  shared_model::crypto::PublicKey pubkey1(std::string("1", 32));
  shared_model::crypto::PublicKey pubkey2(std::string("2", 32));

  auto txn1 =
      TestTransactionBuilder()
          .creatorAccountId("admin1")
          .createRole("user",
                      std::set<std::string>{
                          can_add_peer, can_create_asset, can_get_my_account})
          .createDomain("domain", "user")
          .createAccount("user1", "domain", pubkey1)
          .build();

  auto txn2 =
      TestTransactionBuilder()
          .creatorAccountId("admin1")
          .createRole("user2",
                      std::set<std::string>{
                          can_add_peer, can_create_asset, can_get_my_account})
          .createDomain("domain2", "user")
          .build();

  auto block = TestBlockBuilder()
                   .transactions(std::vector<shared_model::proto::Transaction>(
                       {txn1, txn2}))
                   .height(1)
                   .prevHash(fake_hash)
                   .txNumber(2)
                   .build();

  apply(storage, block);

  // TODO: 31.10.2017 luckychess move tx3hash case into a separate test after
  // ametsuchi_test redesign
  auto tx1hash = txn1.hash();
  auto tx2hash = txn2.hash();
  auto tx3hash = shared_model::crypto::Hash("some garbage");

  auto tx1check = *blocks->getTxByHashSync(tx1hash);

  auto tx1 = *blocks->getTxByHashSync(tx1hash);
  auto tx2 = *blocks->getTxByHashSync(tx2hash);

  ASSERT_EQ(*tx1.operator->(), txn1);
  ASSERT_EQ(*tx2.operator->(), txn2);
  ASSERT_EQ(blocks->getTxByHashSync(tx3hash), boost::none);
}

/**
 * @given initialized storage for ordering service
 * @when save proposal height
 * @then load proposal height and ensure it is correct
 */
TEST_F(AmetsuchiTest, OrderingServicePersistentStorageTest) {
  std::shared_ptr<iroha::ametsuchi::PostgresOrderingServicePersistentState>
      ordering_state;
  iroha::ametsuchi::PostgresOrderingServicePersistentState::create(pgopt_)
      .match([&](iroha::expected::Value<std::shared_ptr<
                     iroha::ametsuchi::PostgresOrderingServicePersistentState>>
                     &_storage) { ordering_state = _storage.value; },
             [](iroha::expected::Error<std::string> &error) {
               FAIL() << "PostgresOrderingServicePersistentState: "
                      << error.error;
             });
  ASSERT_TRUE(ordering_state);

  ordering_state->resetState();
  ASSERT_EQ(2, ordering_state->loadProposalHeight().value());
  ASSERT_TRUE(ordering_state->saveProposalHeight(11));
  ASSERT_EQ(11, ordering_state->loadProposalHeight().value());
  ASSERT_TRUE(ordering_state->saveProposalHeight(33));
  ASSERT_EQ(33, ordering_state->loadProposalHeight().value());
  ordering_state->resetState();
  ASSERT_EQ(2, ordering_state->loadProposalHeight().value());
}

/**
 * @given initialized storage for ordering service
 * @when save proposal height
 * @then load proposal height and ensure it is correct
 */
TEST_F(AmetsuchiTest, OrderingServicePersistentStorageRestartTest) {
  std::shared_ptr<iroha::ametsuchi::PostgresOrderingServicePersistentState>
      ordering_state;
  iroha::ametsuchi::PostgresOrderingServicePersistentState::create(pgopt_)
      .match([&](iroha::expected::Value<std::shared_ptr<
                     iroha::ametsuchi::PostgresOrderingServicePersistentState>>
                     &_storage) { ordering_state = _storage.value; },
             [](iroha::expected::Error<std::string> &error) {
               FAIL() << "PostgresOrderingServicePersistentState: "
                      << error.error;
             });
  ASSERT_TRUE(ordering_state);

  ordering_state->resetState();
  ASSERT_EQ(2, ordering_state->loadProposalHeight().value());
  ASSERT_TRUE(ordering_state->saveProposalHeight(11));
  ASSERT_EQ(11, ordering_state->loadProposalHeight().value());

  // restart Ordering Service Storage
  ordering_state.reset();
  iroha::ametsuchi::PostgresOrderingServicePersistentState::create(pgopt_)
      .match([&](iroha::expected::Value<std::shared_ptr<
                     iroha::ametsuchi::PostgresOrderingServicePersistentState>>
                     &_storage) { ordering_state = _storage.value; },
             [](iroha::expected::Error<std::string> &error) {
               FAIL() << "PostgresOrderingServicePersistentState: "
                      << error.error;
             });
  ASSERT_TRUE(ordering_state);
  ASSERT_EQ(11, ordering_state->loadProposalHeight().value());
}

/**
 * @given 2 different initialized storages for ordering service
 * @when save proposal height to the first one
 * @then the state is consistent
 */
TEST_F(AmetsuchiTest,
       OrderingServicePersistentStorageDifferentConnectionsTest) {
  std::shared_ptr<iroha::ametsuchi::PostgresOrderingServicePersistentState>
      ordering_state_1;
  iroha::ametsuchi::PostgresOrderingServicePersistentState::create(pgopt_)
      .match([&](iroha::expected::Value<std::shared_ptr<
                     iroha::ametsuchi::PostgresOrderingServicePersistentState>>
                     &_storage) { ordering_state_1 = _storage.value; },
             [](iroha::expected::Error<std::string> &error) {
               FAIL() << "PostgresOrderingServicePersistentState: "
                      << error.error;
             });
  ASSERT_TRUE(ordering_state_1);

  std::shared_ptr<iroha::ametsuchi::PostgresOrderingServicePersistentState>
      ordering_state_2;
  iroha::ametsuchi::PostgresOrderingServicePersistentState::create(pgopt_)
      .match([&](iroha::expected::Value<std::shared_ptr<
                     iroha::ametsuchi::PostgresOrderingServicePersistentState>>
                     &_storage) { ordering_state_2 = _storage.value; },
             [](iroha::expected::Error<std::string> &error) {
               FAIL() << "PostgresOrderingServicePersistentState: "
                      << error.error;
             });
  ASSERT_TRUE(ordering_state_2);

  ordering_state_2->resetState();
  ASSERT_EQ(2, ordering_state_1->loadProposalHeight().value());
  ASSERT_EQ(2, ordering_state_2->loadProposalHeight().value());
  ASSERT_TRUE(ordering_state_1->saveProposalHeight(11));
  ASSERT_EQ(11, ordering_state_1->loadProposalHeight().value());
  ASSERT_EQ(11, ordering_state_2->loadProposalHeight().value());

  ordering_state_2->resetState();
  ASSERT_EQ(2, ordering_state_1->loadProposalHeight().value());
  ASSERT_EQ(2, ordering_state_2->loadProposalHeight().value());
  ASSERT_TRUE(ordering_state_2->saveProposalHeight(42));
  ASSERT_EQ(42, ordering_state_1->loadProposalHeight().value());
  ASSERT_EQ(42, ordering_state_2->loadProposalHeight().value());
}
