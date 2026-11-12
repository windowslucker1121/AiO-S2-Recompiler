// SPDX-License-Identifier: GPL-3.0-or-later
// Offline network services (S3c): what a console without a connection, friends or BCAT data answers. The request bytes and the
// expected values are written out from Switchbrew's command ids and result codes (result = module | (description << 9)), not read from
// the service under test.
#include "svc_test_util.h"

#include <chrono>
#include <thread>

NXTEST_GROUP(svc_network)

using namespace nxrt;

namespace {
constexpr uint32_t kNifm300 = 0x2586E;          // nifm 300 NoInternetConnection: 110 | (300 << 9)
constexpr uint32_t kLdn65 = 0x82CB;             // ldn 65 ConnectNotFound: 203 | (65 << 9)
constexpr uint32_t kNtc1000 = 0x7D074;          // time 1000 NetworkTimeNotAvailable: 116 | (1000 << 9)
constexpr uint32_t kSslInvalidSocket = 0xD47B;  // ssl 106 InvalidSocket: 123 | (106 << 9)

// Waits (no timeout) on a copied event handle: true when it is signalled.
bool signalled(uint32_t h) {
    int32_t idx = -1;
    return kernel::wait_synchronization(&h, 1, 0, &idx) == 0;
}
}  // namespace

NXTEST(nifm_no_connection) {
    nxsvc::Env env;
    nxsvc::Session nifm = nxsvc::connect("nifm:u");
    REQUIRE(nifm.valid());
    // CreateGeneralService(pid, u64 reserved) -> IGeneralService (command 5)
    nxsvc::Reply g = nifm.call(5, nxsvc::Args().with_pid().u64(0));
    REQUIRE(g.ok());
    nxsvc::Session gen = nifm.sub(g);
    REQUIRE(gen.valid());
    // GetCurrentIpAddress (12): no address without a connection
    CHECK_EQ(gen.call(12).result, kNifm300);
    // IsAnyForegroundRequestAccepted (22): false, plain u8 output
    nxsvc::Reply fg = gen.call(22);
    CHECK_EQ(fg.result, 0u);
    CHECK_EQ(fg.get<uint8_t>(0), 0);
    // CreateRequest(s32 preset = 2) (4) -> IRequest; GetRequestState (0) is 1 (Free); GetResult (1) is the no-connection result
    nxsvc::Session req = gen.sub(gen.call(4, nxsvc::Args().i32(2)));
    REQUIRE(req.valid());
    CHECK_EQ(req.call(0).get<uint32_t>(0), 1u);
    CHECK_EQ(req.call(1).result, kNifm300);
    // GetSystemEventReadableHandles (2): two copies of events that never signal
    nxsvc::Reply ev = req.call(2);
    REQUIRE(ev.copy.size() == 2);
    CHECK(!signalled(ev.copy[0]));
    CHECK(!signalled(ev.copy[1]));
    for (uint32_t h : ev.copy) kernel::handle_close(h);
}

// Sockets exist and do nothing (the game's gRPC probes IPv6 with a UDP socket and aborts when Socket fails): Socket hands out a
// descriptor, the commands that would need a network fail with ENETDOWN, Close frees it once.
NXTEST(bsd_socket_exists_but_has_no_network) {
    nxsvc::Env env;
    nxsvc::Session bsd = nxsvc::connect("bsd:u");
    REQUIRE(bsd.valid());
    // Socket(i32 domain = AF_INET 2, i32 type = SOCK_STREAM 1, i32 protocol 0) -> (i32 ret, u32 errno): a descriptor, errno 0
    nxsvc::Reply r = bsd.call(2, nxsvc::Args().i32(2).i32(1).i32(0));
    CHECK_EQ(r.result, 0u);
    const int32_t fd = r.get<int32_t>(0);
    CHECK(fd >= 3);                       // 0..2 are the standard streams on the console's bsd too
    CHECK_EQ(r.get<uint32_t>(4), 0u);
    // a second socket is another descriptor
    nxsvc::Reply r2 = bsd.call(2, nxsvc::Args().i32(10).i32(2).i32(0));   // AF_INET6, SOCK_DGRAM
    const int32_t fd2 = r2.get<int32_t>(0);
    CHECK(fd2 >= 3 && fd2 != fd);
    // Connect(i32 fd, sockaddr in an A buffer) -> (-1, ENETDOWN): there is no network
    uint8_t addr[16] = {0x10, 2};
    nxsvc::Reply cn = bsd.call(14, nxsvc::Args().i32(fd).buf_a(addr, sizeof addr));
    CHECK_EQ(cn.result, 0u);
    CHECK_EQ(cn.get<int32_t>(0), -1);
    CHECK_EQ(cn.get<uint32_t>(4), 100u);
    // Fcntl(fd, F_GETFL 3, 0) -> O_NONBLOCK (4)
    nxsvc::Reply fl = bsd.call(20, nxsvc::Args().i32(fd).i32(3).i32(0));
    CHECK_EQ(fl.get<int32_t>(0), 4);
    CHECK_EQ(fl.get<uint32_t>(4), 0u);
    // Close(i32 fd) -> (i32 ret, u32 errno): (0, 0), then the descriptor is gone: (-1, EBADF 9)
    nxsvc::Reply c = bsd.call(26, nxsvc::Args().i32(fd));
    CHECK_EQ(c.result, 0u);
    CHECK_EQ(c.get<int32_t>(0), 0);
    CHECK_EQ(c.get<uint32_t>(4), 0u);
    c = bsd.call(26, nxsvc::Args().i32(fd));
    CHECK_EQ(c.get<int32_t>(0), -1);
    CHECK_EQ(c.get<uint32_t>(4), 9u);
    fl = bsd.call(20, nxsvc::Args().i32(fd).i32(3).i32(0));
    CHECK_EQ(fl.get<int32_t>(0), -1);
    CHECK_EQ(fl.get<uint32_t>(4), 9u);
    CHECK_EQ(bsd.call(26, nxsvc::Args().i32(fd2)).get<int32_t>(0), 0);
}

// EventFd (31) is Linux's eventfd: Write adds the 8-byte value, Read returns the counter and clears it (EFD_SEMAPHORE: 1 at a time), an empty
// one answers EAGAIN; Poll reports POLLIN once the counter is non-zero, a socket never, an unknown descriptor POLLNVAL.
NXTEST(bsd_eventfd_and_poll) {
    nxsvc::Env env;
    nxsvc::Session bsd = nxsvc::connect("bsd:u");
    REQUIRE(bsd.valid());
    // EventFd(u64 initval = 0, u32 flags = 0) -> (fd, 0)
    nxsvc::Reply e = bsd.call(31, nxsvc::Args().u64(0).u32(0));
    CHECK_EQ(e.result, 0u);
    const int32_t efd = e.get<int32_t>(0);
    CHECK(efd >= 3);
    CHECK_EQ(e.get<uint32_t>(4), 0u);
    const int32_t sock = bsd.call(2, nxsvc::Args().i32(2).i32(2).i32(0)).get<int32_t>(0);
    CHECK(sock >= 3 && sock != efd);
    uint64_t v = 0xEEEEEEEEEEEEEEEEull;
    // Read(fd) -> B buffer: empty -> (-1, EAGAIN 11), the buffer untouched
    nxsvc::Reply rd = bsd.call(25, nxsvc::Args().i32(efd).buf_b(&v, 8));
    CHECK_EQ(rd.get<int32_t>(0), -1);
    CHECK_EQ(rd.get<uint32_t>(4), 11u);
    CHECK_EQ(v, 0xEEEEEEEEEEEEEEEEull);
    // Poll(u32 nfds, i32 timeout) with pollfd {i32 fd, i16 events, i16 revents} in an A buffer and back in a B buffer
    struct PollFd { int32_t fd; int16_t events, revents; };
    PollFd in[3] = {{efd, 0x1, 0}, {sock, 0x1, 0}, {0x7777, 0x1, 0}}, out[3] = {};
    nxsvc::Reply p = bsd.call(6, nxsvc::Args().u32(2).i32(0).buf_a(in, 2 * sizeof(PollFd)).buf_b(out, 2 * sizeof(PollFd)));
    CHECK_EQ(p.get<int32_t>(0), 0);       // nothing ready, timeout 0 returns at once
    CHECK_EQ(out[0].revents, 0);
    CHECK_EQ(out[1].revents, 0);
    // Write(fd, 8 bytes = 2) -> (8, 0); then Poll: the eventfd is readable, the socket is not, the unknown descriptor is invalid
    const uint64_t two = 2;
    nxsvc::Reply wr = bsd.call(24, nxsvc::Args().i32(efd).buf_a(&two, 8));
    CHECK_EQ(wr.get<int32_t>(0), 8);
    CHECK_EQ(wr.get<uint32_t>(4), 0u);
    p = bsd.call(6, nxsvc::Args().u32(3).i32(0).buf_a(in, sizeof in).buf_b(out, sizeof out));
    CHECK_EQ(p.get<int32_t>(0), 2);
    CHECK_EQ(out[0].revents, 0x1);        // POLLIN
    CHECK_EQ(out[1].revents, 0);
    CHECK_EQ(out[2].revents, 0x20);       // POLLNVAL
    // Read takes the whole counter, then the eventfd is empty again
    rd = bsd.call(25, nxsvc::Args().i32(efd).buf_b(&v, 8));
    CHECK_EQ(rd.get<int32_t>(0), 8);
    CHECK_EQ(v, 2ull);
    rd = bsd.call(25, nxsvc::Args().i32(efd).buf_b(&v, 8));
    CHECK_EQ(rd.get<uint32_t>(4), 11u);
    // EFD_SEMAPHORE (1): each Read takes 1
    const int32_t sem = bsd.call(31, nxsvc::Args().u64(2).u32(1)).get<int32_t>(0);
    CHECK(sem >= 3);
    for (int i = 0; i < 2; ++i) {
        rd = bsd.call(25, nxsvc::Args().i32(sem).buf_b(&v, 8));
        CHECK_EQ(rd.get<int32_t>(0), 8);
        CHECK_EQ(v, 1ull);
    }
    CHECK_EQ(bsd.call(25, nxsvc::Args().i32(sem).buf_b(&v, 8)).get<uint32_t>(4), 11u);
    // a Poll with a timeout returns when a Write comes from another thread, not at the end of the timeout
    std::thread writer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        nxsvc::Session b2 = nxsvc::connect("bsd:u");
        b2.call(24, nxsvc::Args().i32(efd).buf_a(&two, 8));
    });
    const auto t0 = std::chrono::steady_clock::now();
    p = bsd.call(6, nxsvc::Args().u32(1).i32(5000).buf_a(in, sizeof(PollFd)).buf_b(out, sizeof(PollFd)));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    writer.join();
    CHECK_EQ(p.get<int32_t>(0), 1);
    CHECK_EQ(out[0].revents, 0x1);
    CHECK_MSG(ms < 4000.0, "Poll waited for its whole timeout although a Write came");
    for (int32_t fd : {efd, sock, sem}) CHECK_EQ(bsd.call(26, nxsvc::Args().i32(fd)).get<int32_t>(0), 0);
}

NXTEST(ssl_context_connection_fails) {
    nxsvc::Env env;
    nxsvc::Session ssl = nxsvc::connect("ssl");
    REQUIRE(ssl.valid());
    // CreateContext(u32 SslVersion, pad, u64 pid placeholder) with the pid -> ISslContext (command 0)
    nxsvc::Session ctx = ssl.sub(ssl.call(0, nxsvc::Args().with_pid().u32(5).pad(4).u64(0)));
    REQUIRE(ctx.valid());
    CHECK_EQ(ctx.call(3).get<uint32_t>(0), 0u);     // GetConnectionCount: none yet
    nxsvc::Session conn = ctx.sub(ctx.call(2));     // CreateConnection (2) -> ISslConnection
    REQUIRE(conn.valid());
    CHECK_EQ(ctx.call(3).get<uint32_t>(0), 1u);
    CHECK_EQ(conn.call(8).result, kSslInvalidSocket);   // DoHandshake: no socket
    CHECK_EQ(conn.call(0, nxsvc::Args().i32(3)).result, kSslInvalidSocket);   // SetSocketDescriptor
    // Closing the connection drops the count again (on_close of the released object)
    conn.close();
    CHECK_EQ(ctx.call(3).get<uint32_t>(0), 0u);
}

NXTEST(nim_eca_creation_succeeds) {
    nxsvc::Env env;
    nxsvc::Session nim = nxsvc::connect("nim:eca");
    REQUIRE(nim.valid());
    services::ServiceEvent stand_in;   // the transfer memory argument: a copy handle the server closes
    const uint32_t h = stand_in.new_handle();
    REQUIRE(h != 0);
    // CreateServerInterface(pid, handle<copy>, u64) (0) -> IShopServiceAccessServer
    nxsvc::Reply r = nim.call(0, nxsvc::Args().with_pid().hcopy(h).u64(0));
    CHECK_EQ(r.result, 0u);
    nxsvc::Session server = nim.sub(r);
    REQUIRE(server.valid());
    nxsvc::Session accessor = server.sub(server.call(0));   // CreateAccessorInterface (0)
    REQUIRE(accessor.valid());
    CHECK_EQ(accessor.call(0).result, 0u);                   // CreateAsyncInterface (0)
    CHECK_EQ(nim.call(4).get<uint8_t>(0), 0);                 // IsLargeResourceAvailable: false
}

NXTEST(friend_empty_list) {
    nxsvc::Env env;
    nxsvc::Session fr = nxsvc::connect("friend:u");
    REQUIRE(fr.valid());
    nxsvc::Session fs = fr.sub(fr.call(0));                  // CreateFriendService (0)
    REQUIRE(fs.valid());
    CHECK_EQ(fs.call(20100).get<int32_t>(0), 0);             // GetFriendCount: 0
    nxsvc::Reply list = fs.call(10101, nxsvc::Args().i32(0));   // GetFriendList: count 0
    CHECK_EQ(list.result, 0u);
    CHECK_EQ(list.get<int32_t>(0), 0);
    nxsvc::Reply done = fs.call(0);                          // GetCompletionEvent: already signalled
    REQUIRE(done.copy.size() == 1);
    CHECK(signalled(done.copy[0]));
    kernel::handle_close(done.copy[0]);
}

NXTEST(pctl_no_restriction) {
    nxsvc::Env env;
    nxsvc::Session pc = nxsvc::connect("pctl");
    REQUIRE(pc.valid());
    nxsvc::Session s = pc.sub(pc.call(0, nxsvc::Args().with_pid()));   // CreateService(pid) (0)
    REQUIRE(s.valid());
    CHECK_EQ(s.call(1031).get<uint8_t>(0), 0);                // IsRestrictionEnabled: false
    CHECK_EQ(s.call(1018).get<uint8_t>(0), 1);                // IsFreeCommunicationAvailable: true
    CHECK_EQ(s.call(1065).get<uint8_t>(0), 1);                // IsStereoVisionPermitted: true
    nxsvc::Reply agc = s.call(1101, nxsvc::Args().u64(2).u64(1));   // CheckAgcPermissionUnderMinimumAge(Uid): permitted
    CHECK_EQ(agc.result, 0u);
    CHECK_EQ(agc.get<uint8_t>(0), 1);
    CHECK_EQ(s.call(1039).get<int32_t>(0), 0);                // GetFreeCommunicationApplicationListCount: 0
    CHECK_EQ(s.call(1001).result, 0u);                        // CheckFreeCommunicationPermission: allowed
}

NXTEST(ldn_state_machine_never_finds_a_network) {
    nxsvc::Env env;
    nxsvc::Session ldn = nxsvc::connect("ldn:u");
    REQUIRE(ldn.valid());
    nxsvc::Session l = ldn.sub(ldn.call(0));                  // CreateUserLocalCommunicationService (0)
    REQUIRE(l.valid());
    CHECK_EQ(l.call(0).get<uint32_t>(0), 0u);                  // GetState: None before Initialize
    CHECK_EQ(l.call(400, nxsvc::Args().with_pid()).result, 0u);   // Initialize(pid) (400) is accepted
    CHECK_EQ(l.call(0).get<uint32_t>(0), 1u);                  // now Initialized
    CHECK_EQ(l.call(102, nxsvc::Args().u16(0)).get<uint16_t>(0), 0);   // Scan: count 0
    CHECK_EQ(l.call(302, nxsvc::Args()).result, kLdn65);       // Connect: ConnectNotFound
    CHECK_EQ(l.call(401).result, 0u);                          // Finalize
    CHECK_EQ(l.call(0).get<uint32_t>(0), 0u);                  // back to None
}

NXTEST(ntc_network_clock_task_fails) {
    nxsvc::Env env;
    nxsvc::Session ntc = nxsvc::connect("ntc");
    REQUIRE(ntc.valid());
    nxsvc::Session t = ntc.sub(ntc.call(0));                  // CreateEnsureNetworkClockAvailabilityService (0)
    REQUIRE(t.valid());
    CHECK_EQ(t.call(0).result, kNtc1000);                     // StartTask: network not available
    CHECK_EQ(t.call(2).result, kNtc1000);                     // GetResult: the same
    nxsvc::Reply fin = t.call(1);                             // GetFinishNotificationEvent: signalled when the task ends
    REQUIRE(fin.copy.size() == 1);
    CHECK(signalled(fin.copy[0]));
    kernel::handle_close(fin.copy[0]);
}

NXTEST(bcat_sync_done_with_empty_cache) {
    nxsvc::Env env;
    nxsvc::Session bcat = nxsvc::connect("bcat:u");
    REQUIRE(bcat.valid());
    nxsvc::Session b = bcat.sub(bcat.call(0, nxsvc::Args().with_pid()));   // CreateBcatService(pid) (0)
    REQUIRE(b.valid());
    nxsvc::Session prog = b.sub(b.call(10100));               // RequestSyncDeliveryCache (10100)
    REQUIRE(prog.valid());
    nxsvc::Reply ev = prog.call(0);                           // GetEvent (0): the sync is over at once
    REQUIRE(ev.copy.size() == 1);
    CHECK(signalled(ev.copy[0]));
    kernel::handle_close(ev.copy[0]);
    // Open a file in the empty cache: path not found (fs 2002)
    nxsvc::Session storage = bcat.sub(bcat.call(1, nxsvc::Args().with_pid()));   // CreateDeliveryCacheStorageService (1)
    REQUIRE(storage.valid());
    nxsvc::Session file = storage.sub(storage.call(0));       // CreateFileService (0)
    REQUIRE(file.valid());
    CHECK_EQ(file.call(0).result, 0x7D402u);                  // Open: 2 | (1002 << 9) = PathNotFound (fs 2002)
}

NXTEST(nsd_environment_and_resolve) {
    nxsvc::Env env;
    nxsvc::Session nsd = nxsvc::connect("nsd:u");
    REQUIRE(nsd.valid());
    char env_id[8] = {};
    CHECK_EQ(nsd.call(11, nxsvc::Args().buf_c(env_id, 8)).result, 0u);   // GetEnvironmentIdentifier: "lp1"
    CHECK(std::memcmp(env_id, "lp1", 3) == 0);
    char name[0x100] = "example.invalid";
    char out[0x100] = {};
    CHECK_EQ(nsd.call(20, nxsvc::Args().buf_x(name, 0x100).buf_c(out, 0x100)).result, 0u);   // Resolve: the name as it was
    CHECK(std::memcmp(out, "example.invalid", 16) == 0);
}

NXTEST(ngct_returns_text_unchanged) {
    nxsvc::Env env;
    nxsvc::Session ngct = nxsvc::connect("ngct:u");
    REQUIRE(ngct.valid());
    char text[] = "hello";
    CHECK_EQ(ngct.call(0, nxsvc::Args().buf_a(text, 5)).get<uint8_t>(0), 0);   // Match: nothing found
    char out[8] = {};
    CHECK_EQ(ngct.call(1, nxsvc::Args().buf_a(text, 5).buf_c(out, 5)).result, 0u);   // Filter: the text back
    CHECK(std::memcmp(out, "hello", 5) == 0);
}

NXTEST(prepo_and_notif_accept) {
    nxsvc::Env env;
    nxsvc::Session prepo = nxsvc::connect("prepo:u");
    REQUIRE(prepo.valid());
    CHECK_EQ(prepo.call(10400).get<uint64_t>(0), 0ull);       // GetSystemSessionId: 0
    CHECK_EQ(prepo.call(10200).result, 0u);                   // RequestImmediateTransmission: accepted
    nxsvc::Session notif = nxsvc::connect("notif:a");
    REQUIRE(notif.valid());
    CHECK_EQ(notif.call(520).get<int32_t>(0), 0);             // ListAlarmSettings: count 0
}

NXTEST(sfdnsres_lookups_fail) {
    nxsvc::Env env;
    nxsvc::Session dns = nxsvc::connect("sfdnsres");
    REQUIRE(dns.valid());
    nxsvc::Reply r = dns.call(6, nxsvc::Args());               // GetAddrInfoRequest: (bsd_errno, gai_error, size)
    CHECK_EQ(r.result, 0u);
    CHECK_EQ(r.get<int32_t>(4), -2);                           // EAI_NONAME
    CHECK_EQ(dns.call(8, nxsvc::Args().with_pid()).get<uint32_t>(0), 1u);   // GetCancelHandleRequest: first handle
}
