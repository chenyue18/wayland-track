#include "relay.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "logger.hpp"

namespace relay {

namespace {

void close_fds(const std::vector<int>& fds) {
    for (int fd : fds) close(fd);
}

ssize_t send_all(int dst, const uint8_t* data, size_t len, const std::vector<int>& fds) {
    struct msghdr msg{};
    struct iovec iov{};
    iov.iov_base = const_cast<uint8_t*>(data);
    iov.iov_len = len;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    std::vector<char> ctl;
    if (!fds.empty()) {
        ctl.resize(CMSG_SPACE(sizeof(int) * fds.size()));
        memset(ctl.data(), 0, ctl.size());
        msg.msg_control = ctl.data();
        msg.msg_controllen = ctl.size();
        struct cmsghdr* cm = CMSG_FIRSTHDR(&msg);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int) * fds.size());
        memcpy(CMSG_DATA(cm), fds.data(), sizeof(int) * fds.size());
    }
    ssize_t n = sendmsg(dst, &msg, MSG_NOSIGNAL);
    if (n < 0) return -1;
    size_t sent = size_t(n);
    while (sent < len) {
        ssize_t w = write(dst, data + sent, len - sent);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += size_t(w);
    }
    return ssize_t(len);
}

}  // namespace

void pump(int src_fd, int dst_fd, int dir, std::shared_ptr<PumpCtx> ctx) {
    try {
        pump_impl(src_fd, dst_fd, dir, ctx);
    } catch (const std::exception& e) {
        logf("转发线程异常(已恢复，不影响其他连接): %s", e.what());
        if (ctx->remaining->fetch_sub(1) == 1) {
            ctx->tracker->close_conn(ctx->conn_id);
            close(ctx->client_fd);
            close(ctx->real_fd);
        }
    } catch (...) {
        logf("转发线程未知异常(已恢复)");
        if (ctx->remaining->fetch_sub(1) == 1) {
            ctx->tracker->close_conn(ctx->conn_id);
            close(ctx->client_fd);
            close(ctx->real_fd);
        }
    }
}

void pump_impl(int src_fd, int dst_fd, int dir, std::shared_ptr<PumpCtx> ctx) {
    auto parser = ctx->parser;
    Tracker* tracker = ctx->tracker.get();
    uint64_t conn_id = ctx->conn_id;
    std::vector<uint8_t> buf(256 << 10);
    std::vector<uint8_t> oob(16 << 10);
    while (true) {
        struct iovec iov{};
        iov.iov_base = buf.data();
        iov.iov_len = buf.size();
        struct msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = oob.data();
        msg.msg_controllen = oob.size();
        ssize_t n = recvmsg(src_fd, &msg, MSG_CMSG_CLOEXEC);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;

        std::vector<int> fds;
        for (struct cmsghdr* cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
            if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
                size_t count = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                const int* data = reinterpret_cast<const int*>(CMSG_DATA(cm));
                for (size_t i = 0; i < count; i++) fds.push_back(data[i]);
            }
        }

        {
            FeedResult fr = parser->feed(dir, buf.data(), size_t(n), int(fds.size()), Clock::now());
            if (!fr.msgs.empty()) tracker->add_msgs(conn_id, std::move(fr.msgs));
        }
        if (send_all(dst_fd, buf.data(), size_t(n), fds) < 0) {
            close_fds(fds);
            break;
        }
        close_fds(fds);
    }
    shutdown(dst_fd, SHUT_WR);
    if (ctx->remaining->fetch_sub(1) == 1) {
        tracker->close_conn(conn_id);
        close(ctx->client_fd);
        close(ctx->real_fd);
    }
}

}  // namespace relay
