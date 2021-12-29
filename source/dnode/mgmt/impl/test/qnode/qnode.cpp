/**
 * @file dnode.cpp
 * @author slguan (slguan@taosdata.com)
 * @brief DNODE module dnode-msg tests
 * @version 0.1
 * @date 2021-12-15
 *
 * @copyright Copyright (c) 2021
 *
 */

#include "base.h"

class DndTestQnode : public ::testing::Test {
 public:
  void SetUp() override {}
  void TearDown() override {}

 public:
  static void SetUpTestSuite() {
    test.Init("/tmp/dnode_test_qnode1", 9071);
    const char* fqdn = "localhost";
    const char* firstEp = "localhost:9071";

    server2.Start("/tmp/dnode_test_qnode2", fqdn, 9072, firstEp);
    server3.Start("/tmp/dnode_test_qnode3", fqdn, 9073, firstEp);
    server4.Start("/tmp/dnode_test_qnode4", fqdn, 9074, firstEp);
    server5.Start("/tmp/dnode_test_qnode5", fqdn, 9075, firstEp);
    taosMsleep(300);
  }

  static void TearDownTestSuite() {
    server2.Stop();
    server3.Stop();
    server4.Stop();
    server5.Stop();
    test.Cleanup();
  }

  static Testbase   test;
  static TestServer server2;
  static TestServer server3;
  static TestServer server4;
  static TestServer server5;
};

Testbase   DndTestQnode::test;
TestServer DndTestQnode::server2;
TestServer DndTestQnode::server3;
TestServer DndTestQnode::server4;
TestServer DndTestQnode::server5;

// TEST_F(DndTestQnode, 01_ShowQnode) {
//   test.SendShowMetaMsg(TSDB_MGMT_TABLE_QNODE, "");
//   CHECK_META("show qnodes", 3);

//   CHECK_SCHEMA(0, TSDB_DATA_TYPE_SMALLINT, 2, "id");
//   CHECK_SCHEMA(1, TSDB_DATA_TYPE_BINARY, TSDB_EP_LEN + VARSTR_HEADER_SIZE, "endpoint");
//   CHECK_SCHEMA(2, TSDB_DATA_TYPE_TIMESTAMP, 8, "create_time");

//   test.SendShowRetrieveMsg();
//   EXPECT_EQ(test.GetShowRows(), 1);

//   CheckInt16(1);
//   CheckBinary("localhost:9071", TSDB_EP_LEN);
//   CheckTimestamp();
// }

TEST_F(DndTestQnode, 02_Create_Qnode_Invalid_Id) {
  {
    int32_t contLen = sizeof(SMCreateQnodeMsg);

    SMCreateQnodeMsg* pReq = (SMCreateQnodeMsg*)rpcMallocCont(contLen);
    pReq->dnodeId = htonl(1);

    SRpcMsg* pMsg = test.SendMsg(TDMT_MND_CREATE_QNODE, pReq, contLen);
    ASSERT_NE(pMsg, nullptr);
    ASSERT_EQ(pMsg->code, 0);

    test.SendShowMetaMsg(TSDB_MGMT_TABLE_QNODE, "");
    CHECK_META("show qnodes", 3);

    CHECK_SCHEMA(0, TSDB_DATA_TYPE_SMALLINT, 2, "id");
    CHECK_SCHEMA(1, TSDB_DATA_TYPE_BINARY, TSDB_EP_LEN + VARSTR_HEADER_SIZE, "endpoint");
    CHECK_SCHEMA(2, TSDB_DATA_TYPE_TIMESTAMP, 8, "create_time");

    test.SendShowRetrieveMsg();
    EXPECT_EQ(test.GetShowRows(), 1);

    CheckInt16(1);
    CheckBinary("localhost:9071", TSDB_EP_LEN);
    CheckTimestamp();
  }
}

// TEST_F(DndTestQnode, 03_Create_Qnode_Invalid_Id) {
//   {
//     int32_t contLen = sizeof(SMCreateQnodeMsg);

//     SMCreateQnodeMsg* pReq = (SMCreateQnodeMsg*)rpcMallocCont(contLen);
//     pReq->dnodeId = htonl(2);

//     SRpcMsg* pMsg = test.SendMsg(TDMT_MND_CREATE_QNODE, pReq, contLen);
//     ASSERT_NE(pMsg, nullptr);
//     ASSERT_EQ(pMsg->code, TSDB_CODE_MND_DNODE_NOT_EXIST);
//   }
// }

// TEST_F(DndTestQnode, 04_Create_Qnode) {
//   {
//     // create dnode
//     int32_t contLen = sizeof(SCreateDnodeMsg);

//     SCreateDnodeMsg* pReq = (SCreateDnodeMsg*)rpcMallocCont(contLen);
//     strcpy(pReq->fqdn, "localhost");
//     pReq->port = htonl(9072);

//     SRpcMsg* pMsg = test.SendMsg(TDMT_MND_CREATE_DNODE, pReq, contLen);
//     ASSERT_NE(pMsg, nullptr);
//     ASSERT_EQ(pMsg->code, 0);

//     taosMsleep(1300);
//     test.SendShowMetaMsg(TSDB_MGMT_TABLE_DNODE, "");
//     test.SendShowRetrieveMsg();
//     EXPECT_EQ(test.GetShowRows(), 2);
//   }

//   {
//     // create qnode
//     int32_t contLen = sizeof(SMCreateQnodeMsg);

//     SMCreateQnodeMsg* pReq = (SMCreateQnodeMsg*)rpcMallocCont(contLen);
//     pReq->dnodeId = htonl(2);

//     SRpcMsg* pMsg = test.SendMsg(TDMT_MND_CREATE_QNODE, pReq, contLen);
//     ASSERT_NE(pMsg, nullptr);
//     ASSERT_EQ(pMsg->code, 0);

//     test.SendShowMetaMsg(TSDB_MGMT_TABLE_QNODE, "");
//     test.SendShowRetrieveMsg();
//     EXPECT_EQ(test.GetShowRows(), 2);

//     CheckInt16(1);
//     CheckInt16(2);
//     CheckBinary("localhost:9071", TSDB_EP_LEN);
//     CheckBinary("localhost:9072", TSDB_EP_LEN);
//     CheckTimestamp();
//     CheckTimestamp();
//   }

//   {
//     // drop qnode
//     int32_t contLen = sizeof(SMDropQnodeMsg);

//     SMDropQnodeMsg* pReq = (SMDropQnodeMsg*)rpcMallocCont(contLen);
//     pReq->dnodeId = htonl(2);

//     SRpcMsg* pMsg = test.SendMsg(TDMT_MND_DROP_QNODE, pReq, contLen);
//     ASSERT_NE(pMsg, nullptr);
//     ASSERT_EQ(pMsg->code, 0);

//     test.SendShowMetaMsg(TSDB_MGMT_TABLE_QNODE, "");
//     test.SendShowRetrieveMsg();
//     EXPECT_EQ(test.GetShowRows(), 1);

//     CheckInt16(1);
//     CheckBinary("localhost:9071", TSDB_EP_LEN);
//     CheckTimestamp();
//   }
// }