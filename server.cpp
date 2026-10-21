#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <cerrno>
#include <csignal>
#include <cctype>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "protocol.h"
#include <unordered_map>
#include <chrono>
#include <sys/resource.h>

using namespace std;

unordered_map<string, string> g_newer;
unordered_map<string, string> g_older;

const size_t MOVE_PER_REQUEST = 128;
long long g_grows = 0;
long long g_requests = 0;

struct Conn
{
    int fd = -1;
    size_t read_pos = 0;
    bool want_read = false;
    bool want_write = false;
    bool want_close = false;
    string incoming;
    string outgoing;
};

void move_some_keys()
{
    size_t moved = 0;
    while (moved < MOVE_PER_REQUEST && !g_older.empty())
    {
        auto node = g_older.extract(g_older.begin());
        g_newer.insert(std::move(node));
        moved++;
    }
    if (g_older.empty() && g_older.bucket_count() > 1)
    {
        g_older = unordered_map<string, string>();
    }
}

void grow_if_needed()
{
    if (!g_older.empty())
        return;
    if (g_newer.size() + 1 <= g_newer.bucket_count() * g_newer.max_load_factor())
        return;
    g_older.swap(g_newer);
    g_grows++;
    g_newer = unordered_map<string, string>();
    g_newer.reserve(g_older.size() * 2);
}

string *db_find(const string &key)
{
    auto it = g_newer.find(key);
    if (it != g_newer.end())
        return &it->second;
    it = g_older.find(key);
    if (it != g_older.end())
        return &it->second;
    return nullptr;
}

void db_set(const string &key, string_view value)
{
    string *old_value = db_find(key);
    if (old_value != nullptr)
    {
        old_value->assign(value.data(), value.size());
        return;
    }
    grow_if_needed();
    g_newer[key].assign(value.data(), value.size());
}

int db_del(const string &key)
{
    return g_newer.erase(key) + g_older.erase(key);
}

void report_stats()
{
    using Clock = chrono::steady_clock;
    static Clock::time_point last_time = Clock::now();
    static long long last_requests = 0;
    static double last_user = 0;
    static double last_sys = 0;

    double elapsed = chrono::duration<double>(Clock::now() - last_time).count();
    if (elapsed < 1.0)
    {
        return;
    }

    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    double user = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6;
    double sys = ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;

    long long handled = g_requests - last_requests;
    if (handled > 0)
    {
        int user_pct = (user - last_user) / elapsed * 100;
        int sys_pct = (sys - last_sys) / elapsed * 100;
        cerr << "[stats] " << (long long)(handled / elapsed) << " req/s   CPU "
             << user_pct + sys_pct << "% = user " << user_pct << "% + sys " << sys_pct << "%" << "   keys " << g_newer.size() + g_older.size() << "   table grew " << g_grows << " times" << endl;
    }

    last_time = Clock::now();
    last_requests = g_requests;
    last_user = user;
    last_sys = sys;
}

void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
    {
        perror("fcntl get failed");
        exit(1);
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        perror("fcntl set failed");
        exit(1);
    }
}

Conn *handle_accept(int server_fd)
{
    sockaddr_in client_addr{};
    socklen_t len = sizeof(client_addr);
    int client_fd = accept(server_fd, (sockaddr *)&client_addr, &len);
    if (client_fd < 0)
    {
        if (errno != EAGAIN)
        {
            perror("accept failed");
        }
        return nullptr;
    }

    cout << "New client " << client_fd << " from "
         << inet_ntoa(client_addr.sin_addr) << ":"
         << ntohs(client_addr.sin_port) << endl;

    set_nonblocking(client_fd);

    Conn *conn = new Conn();
    conn->fd = client_fd;
    conn->want_read = true;
    return conn;
}

string to_lower(string s)
{
    for (char &c : s)
    {
        c = tolower((unsigned char)c);
    }
    return s;
}

void do_request(const vector<string_view> &cmd, string &out)
{
    string name = to_lower(string(cmd[0]));
    move_some_keys();
    if (name == "ping")
    {
        reply_simple(out, "PONG");
    }
    else if (name == "get")
    {
        if (cmd.size() != 2)
        {
            reply_error(out, "ERR wrong number of arguments for 'get' command");
            return;
        }
        string *value = db_find(string(cmd[1]));
        if (value == nullptr)
            reply_null(out);
        else
            reply_bulk(out, *value);
    }
    else if (name == "set")
    {
        if (cmd.size() != 3)
        {
            reply_error(out, "ERR wrong number of arguments for 'set' command");
            return;
        }
        db_set(string(cmd[1]), cmd[2]);
        reply_simple(out, "OK");
    }
    else if (name == "del")
    {
        if (cmd.size() < 2)
        {
            reply_error(out, "ERR wrong number of arguments for 'del' command");
            return;
        }
        long long deleted = 0;
        for (size_t i = 1; i < cmd.size(); i++)
        {
            deleted += db_del(string(cmd[i]));
        }
        reply_integer(out, deleted);
    }
    else
    {
        reply_error(out, "ERR unknown command '" + string(cmd[0]) + "'");
    }
}

string short_text(const string &s)
{
    if (s.size() <= 40)
        return s;
    return s.substr(0, 40) + "... (" + to_string(s.size()) + " bytes";
}

bool try_one_message(Conn *conn)
{
    vector<string_view> cmd;
    size_t consumed = 0;
    int r = parse_resp_request(conn->incoming, cmd, consumed, conn->read_pos);

    if (r == PARSE_INCOMPLETE)
    {
        return false;
    }
    if (r == PARSE_ERROR)
    {
        cerr << "Client " << conn->fd << ": protocol error" << endl;
        conn->want_close = true;
        return false;
    }
    do_request(cmd, conn->outgoing);
    g_requests++;
    conn->read_pos += consumed;
    return true;
}

void handle_write(Conn *conn)
{
    ssize_t rv = write(conn->fd, conn->outgoing.data(), conn->outgoing.size());

    if (rv < 0 && errno == EAGAIN)
    {
        return;
    }

    if (rv < 0)
    {
        perror("write failed");
        conn->want_close = true;
        return;
    }
    conn->outgoing.erase(0, rv);
    if (conn->outgoing.empty())
    {
        conn->want_write = false;
        conn->want_read = true;
    }
}

void handle_read(Conn *conn)
{
    char buf[64 * 1024];
    ssize_t rv = read(conn->fd, buf, sizeof(buf));

    if (rv < 0 && errno == EAGAIN)
    {
        return;
    }

    if (rv < 0)
    {
        perror("read failed");
        conn->want_close = true;
        return;
    }
    if (rv == 0)
    {
        cout << "Client " << conn->fd << " left the chat." << endl;
        conn->want_close = true;
        return;
    }

    conn->incoming.append(buf, rv);

    while (try_one_message(conn))
        ;
    if (conn->read_pos > 0)
    {
        conn->incoming.erase(0, conn->read_pos);
        conn->read_pos = 0;
    }

    if (!conn->outgoing.empty())
    {
        conn->want_read = false;
        conn->want_write = true;
        handle_write(conn);
    }
}

int main(int argc, char *argv[])
{
    bool show_stats = (argc > 1 && string(argv[1]) == "--stats");
    signal(SIGPIPE, SIG_IGN);

    // 1. create socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        perror("socket failed");
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 2. bind
    sockaddr_in addr{};
    addr.sin_family = AF_INET;         // IPv4
    addr.sin_port = htons(1234);       // port
    addr.sin_addr.s_addr = INADDR_ANY; // accept connection to any IP

    if (::bind(server_fd, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind failed");
        return 1;
    }

    // set to non_blocking mode
    set_nonblocking(server_fd);

    // 3. listen
    if (listen(server_fd, SOMAXCONN) < 0)
    {
        perror("listen failed");
        return 1;
    }

    cout << "key-value server is running on port " << ntohs(addr.sin_port) << "..." << endl;

    vector<Conn *> fd2conn;
    vector<pollfd> poll_args;

    while (true)
    {
        poll_args.clear();
        poll_args.push_back({server_fd, POLLIN, 0});

        for (Conn *conn : fd2conn)
        {
            if (conn == nullptr)
                continue;
            pollfd pfd = {conn->fd, POLLERR, 0};
            if (conn->want_read)
                pfd.events |= POLLIN;
            if (conn->want_write)
                pfd.events |= POLLOUT;
            poll_args.push_back(pfd);
        }

        int timeout_ms = show_stats ? 1000 : -1;
        int rv = poll(poll_args.data(), (nfds_t)poll_args.size(), timeout_ms);
        if (rv < 0 && errno == EINTR)
            continue;
        if (rv < 0)
        {
            perror("poll failed");
            return 1;
        }

        if (poll_args[0].revents)
        {
            while (Conn *conn = handle_accept(server_fd))
            {
                if (fd2conn.size() <= (size_t)conn->fd)
                    fd2conn.resize(conn->fd + 1, nullptr);
                fd2conn[conn->fd] = conn;
            }
        }

        for (size_t i = 1; i < poll_args.size(); i++)
        {
            short ready = poll_args[i].revents;
            if (ready == 0)
            {
                continue;
            }

            Conn *conn = fd2conn[poll_args[i].fd];

            if (ready & POLLIN)
                handle_read(conn);
            if (ready & POLLOUT)
                handle_write(conn);

            bool hung_up = (ready & POLLHUP) && !(ready & POLLIN);

            if ((ready & POLLERR) || hung_up || conn->want_close)
            {
                close(conn->fd);
                fd2conn[conn->fd] = nullptr;
                delete conn;
            }
        }
        if (show_stats)
        {
            report_stats();
        }
    }
    close(server_fd);
    return 0;
}
