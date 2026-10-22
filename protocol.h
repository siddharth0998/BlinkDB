#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <string_view>

using namespace std;

const size_t MAX_MSG = 32 << 20; // 32MB
const size_t MAX_ARGS = 200 * 1000;

const int PARSE_OK = 1;
const int PARSE_INCOMPLETE = 0;
const int PARSE_ERROR = -1;

int read_number_line(string_view buf, size_t &pos, char prefix, long long &out)
{
    if (pos >= buf.size())
    {
        return PARSE_INCOMPLETE;
    }
    if (buf[pos] != prefix)
    {
        return PARSE_ERROR;
    }
    long long n = 0;
    size_t digits = 0;
    size_t i = pos + 1;

    while (i < buf.size() && buf[i] != '\r')
    {
        char c = buf[i];
        if (c < '0' || c > '9' || digits == 18)
            return PARSE_ERROR;
        n = n * 10 + (c - '0');
        digits++;
        i++;
    }
    if (i + 1 >= buf.size())
        return PARSE_INCOMPLETE;
    if (digits == 0 || buf[i + 1] != '\n')
        return PARSE_ERROR;

    out = n;
    pos = i + 2;
    return PARSE_OK;
}

int parse_resp_request(string_view buf, vector<string_view> &cmd, size_t &consumed, size_t &start)
{
    cmd.clear();
    size_t pos = start;
    long long count = 0;
    int r = read_number_line(buf, pos, '*', count);
    if (r != PARSE_OK)
        return r;
    if (count < 1 || count > (long long)MAX_ARGS)
        return PARSE_ERROR;
    for (long long i = 0; i < count; i++)
    {
        long long len = 0;
        r = read_number_line(buf, pos, '$', len);
        if (r != PARSE_OK)
            return r;
        if (len > (long long)MAX_MSG)
            return PARSE_ERROR;
        if (buf.size() < pos + len + 2)
            return PARSE_INCOMPLETE;
        if (buf[pos + len] != '\r' || buf[pos + len + 1] != '\n')
            return PARSE_ERROR;
        cmd.push_back(buf.substr(pos, len));
        pos += len + 2;
    }
    consumed = pos - start;
    return PARSE_OK;
}

void reply_simple(string &out, string_view s)
{
    out += '+';
    out.append(s.data(), s.size());
    out += "\r\n";
}

void reply_error(string &out, string_view msg)
{
    out += "-";
    out.append(msg.data(), msg.size());
    out += "\r\n";
}

void reply_integer(string &out, long long n)
{
    out += ":" + to_string(n) + "\r\n";
}

void reply_bulk(string &out, string_view s)
{
    out += "$";
    out += to_string(s.size());
    out += "\r\n";
    out.append(s.data(), s.size());
    out += "\r\n";
}

void reply_null(string &out)
{
    out += "$-1\r\n";
}