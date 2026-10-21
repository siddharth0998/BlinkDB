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

int read_number_line(const string &buf, size_t &pos, char prefix, long long &out)
{
    if (pos >= buf.size())
    {
        return PARSE_INCOMPLETE;
    }
    if (buf[pos] != prefix)
    {
        return PARSE_ERROR;
    }
    size_t end = buf.find("\r\n", pos);
    if (end == string::npos)
    {
        if (buf.size() - pos > 32)
        {
            return PARSE_ERROR;
        }
        return PARSE_INCOMPLETE;
    }
    string digits = buf.substr(pos + 1, end - pos - 1);
    if (digits.empty() || digits.size() > 18)
    {
        return PARSE_ERROR;
    }
    for (char c : digits)
    {
        if (c < '0' || c > '9')
        {
            return PARSE_ERROR;
        }
    }
    out = stoll(digits);
    pos = end + 2;
    return PARSE_OK;
}

int parse_resp_request(const string &buf, vector<string_view> &cmd, size_t &consumed, size_t &start)
{
    size_t pos = start;
    long long count = 0;
    int r = read_number_line(buf, pos, '*', count);
    if (r != PARSE_OK)
    {
        return r;
    }
    if (count < 1 || count > (long long)MAX_ARGS)
    {
        return PARSE_ERROR;
    }

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
        cmd.push_back(string_view(buf.data() + pos, len));
        pos += len + 2;
    }
    consumed = pos - start;
    return PARSE_OK;
}

void reply_simple(string &out, const string &s)
{
    out += "+" + s + "\r\n";
}

void reply_error(string &out, const string &msg)
{
    out += "-" + msg + "\r\n";
}

void reply_integer(string &out, long long n)
{
    out += ":" + to_string(n) + "\r\n";
}

void reply_bulk(string &out, const string &s)
{
    out += "$" + to_string(s.size()) + "\r\n" + s + "\r\n";
}

void reply_null(string &out)
{
    out += "$-1\r\n";
}