#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include "string_utils.h"

class APIAcl {
public:
  // Singleton
  static APIAcl& instance() {
    static APIAcl inst;
    return inst;
  }

  // Class-level configuration (thread-safe)
  void set_disallowed_dest_cidrs(const std::string&dest_cidr_list) {
    std::vector<std::string> cidrs;
    StringUtils::split(dest_cidr_list, cidrs, ",");

    std::vector<Cidr> parsed;
    parsed.reserve(cidrs.size());
    for (const auto& c : cidrs) {
      Cidr out;
      if (parse_cidr_v4(c, out)) parsed.push_back(out);
    }
    std::lock_guard<std::mutex> lk(cfg_mu_);
    disallowed_dest_cidrs_ = std::move(parsed);
  }

  void set_rate_limit_10s(std::size_t limit) {
    rate_limit_10s_.store(limit, std::memory_order_relaxed);
  }

  // Returns true if:
  //  - src_ip is in allowed_ips AND
  //  - dest_url host does NOT resolve into any disallowed CIDR
  bool is_allowed(const std::string& src_ip,
                  const std::string& dest_url,
                  const std::vector<std::string>& allowed_src_ips) {
    // 1) Throttle (class-level)
    if (!throttle_ok_()) return false;

    // 2) Take a snapshot of the class-level destination ACL.
    std::vector<Cidr> disallowed_dest_cidrs_copy;
    {
      std::lock_guard<std::mutex> lk(cfg_mu_);
      disallowed_dest_cidrs_copy = disallowed_dest_cidrs_;
    }

    // 3) src_ip must be in allowed_ips when a source allowlist is configured.
    if (!allowed_src_ips.empty()) {
      IpAddr src;
      if (!parse_ip(src_ip, src)) {
        return false;
      }

      bool src_allowed = false;
      for (const auto& ip : allowed_src_ips) {
        IpAddr allowed_ip;
        if (!parse_ip(ip, allowed_ip)) {
          return false;
        }

        if (same_ip_(src, allowed_ip)) {
          src_allowed = true;
        }
      }

      if (!src_allowed) {
        return false;
      }
    }

    // 4) URL host must NOT be in disallowed CIDRs
    const std::string host = extract_host_(dest_url);
    if (host.empty()) {
      return false;
    }

    std::vector<IpAddr> host_ips;
    if (!host_to_ips_(host, host_ips)) {
      return false; // fail-closed if can't evaluate
    }

    for (const auto& hip : host_ips) {
      if (ip_in_any_cidr_(hip, disallowed_dest_cidrs_copy)) {
        return false;
      }
    }

    return true;
  }

private:
  APIAcl() : rate_limit_10s_(0) {}
  APIAcl(const APIAcl&) = delete;
  APIAcl& operator=(const APIAcl&) = delete;

  struct IpAddr {
    sa_family_t family = AF_UNSPEC;
    uint32_t v4 = 0; // host order
    std::array<uint8_t, 16> v6 = {};
  };

  struct Cidr {
    IpAddr network;
    uint8_t prefix_len = 0;
  };

  // ---- Throttling (fixed 10s rolling window) ----
  bool throttle_ok_() {
    const std::size_t limit = rate_limit_10s_.load(std::memory_order_relaxed);
    if (limit == 0) return true; // 0 == unlimited

    using clock = std::chrono::steady_clock;
    const auto now = clock::now();
    const auto window = std::chrono::seconds(10);

    std::lock_guard<std::mutex> lk(rate_mu_);
    while (!hits_.empty() && (now - hits_.front()) >= window) {
      hits_.pop_front();
    }

    if (hits_.size() >= limit) {
      return false;
    }
    hits_.push_back(now);
    return true;
  }

  // ---- IP + CIDR helpers ----
  static std::string unbracket_ipv6_(const std::string& ip) {
    if (ip.size() >= 2 && ip.front() == '[' && ip.back() == ']') {
      return ip.substr(1, ip.size() - 2);
    }
    return ip;
  }

  static std::string normalize_ipv4_(const std::string& ip) {
    static constexpr const char* ipv4_mapped_prefix = "::ffff:";
    static constexpr std::size_t ipv4_mapped_prefix_len = 7;

    if (ip.size() > ipv4_mapped_prefix_len) {
      bool is_ipv4_mapped = true;
      for (std::size_t i = 0; i < ipv4_mapped_prefix_len; i++) {
        char ch = ip[i];
        if (ch >= 'A' && ch <= 'Z') {
          ch = static_cast<char>(ch - 'A' + 'a');
        }
        if (ch != ipv4_mapped_prefix[i]) {
          is_ipv4_mapped = false;
          break;
        }
      }

      if (!is_ipv4_mapped) {
        return ip;
      }

      return ip.substr(ipv4_mapped_prefix_len);
    }

    return ip;
  }

  static bool parse_ipv4(const std::string& ip, uint32_t& out_host_order) {
    const std::string normalized = normalize_ipv4_(unbracket_ipv6_(ip));
    in_addr a;
    if (::inet_pton(AF_INET, normalized.c_str(), &a) != 1) return false;
    out_host_order = ntohl(a.s_addr);
    return true;
  }

  static bool is_ipv4_mapped_ipv6_(const in6_addr& ip) {
    for (size_t i = 0; i < 10; i++) {
      if (ip.s6_addr[i] != 0) return false;
    }

    return ip.s6_addr[10] == 0xff && ip.s6_addr[11] == 0xff;
  }

  static bool parse_ip(const std::string& ip, IpAddr& out) {
    const std::string unbracketed = unbracket_ipv6_(ip);

    uint32_t v4 = 0;
    if (parse_ipv4(unbracketed, v4)) {
      out.family = AF_INET;
      out.v4 = v4;
      out.v6 = {};
      return true;
    }

    in6_addr a6;
    if (::inet_pton(AF_INET6, unbracketed.c_str(), &a6) != 1) {
      return false;
    }

    if (is_ipv4_mapped_ipv6_(a6)) {
      out.family = AF_INET;
      out.v4 = (static_cast<uint32_t>(a6.s6_addr[12]) << 24) |
               (static_cast<uint32_t>(a6.s6_addr[13]) << 16) |
               (static_cast<uint32_t>(a6.s6_addr[14]) << 8) |
               static_cast<uint32_t>(a6.s6_addr[15]);
      out.v6 = {};
      return true;
    }

    out.family = AF_INET6;
    out.v4 = 0;
    for (size_t i = 0; i < out.v6.size(); i++) {
      out.v6[i] = a6.s6_addr[i];
    }
    return true;
  }

  static bool parse_cidr_v4(const std::string& cidr, Cidr& out) {
    return parse_cidr(cidr, out);
  }

  static bool parse_cidr(const std::string& cidr, Cidr& out) {
    // "a.b.c.d/prefix" or "2001:db8::/32"
    const auto slash = cidr.find('/');
    if (slash == std::string::npos) return false;

    const std::string ip_part = cidr.substr(0, slash);
    const std::string pre_part = cidr.substr(slash + 1);

    char* end = nullptr;
    long prefix = std::strtol(pre_part.c_str(), &end, 10);
    if (!end || *end != '\0') return false;
    IpAddr ip;
    if (!parse_ip(ip_part, ip)) return false;

    const long max_prefix = ip.family == AF_INET ? 32 : 128;
    if (prefix < 0 || prefix > max_prefix) return false;

    out.network = ip;
    out.prefix_len = static_cast<uint8_t>(prefix);
    return true;
  }

  static bool same_ip_(const IpAddr& a, const IpAddr& b) {
    if (a.family != b.family) return false;
    if (a.family == AF_INET) return a.v4 == b.v4;
    return a.v6 == b.v6;
  }

  static bool ipv6_prefix_match_(const std::array<uint8_t, 16>& ip,
                                 const std::array<uint8_t, 16>& network,
                                 uint8_t prefix_len) {
    const size_t full_bytes = prefix_len / 8;
    const uint8_t remaining_bits = prefix_len % 8;

    for (size_t i = 0; i < full_bytes; i++) {
      if (ip[i] != network[i]) return false;
    }

    if (remaining_bits == 0) return true;

    const uint8_t mask = static_cast<uint8_t>(0xff << (8 - remaining_bits));
    return (ip[full_bytes] & mask) == (network[full_bytes] & mask);
  }

  static bool ip_in_cidr_(const IpAddr& ip, const Cidr& c) {
    if (ip.family != c.network.family) return false;

    if (ip.family == AF_INET) {
      uint32_t mask = 0;
      if (c.prefix_len != 0) {
        mask = 0xFFFFFFFFu << (32 - c.prefix_len);
      }
      return (ip.v4 & mask) == (c.network.v4 & mask);
    }

    return ipv6_prefix_match_(ip.v6, c.network.v6, c.prefix_len);
  }

  static bool ip_in_any_cidr_(const IpAddr& ip, const std::vector<Cidr>& cidrs) {
    for (const auto& c : cidrs) {
      if (ip_in_cidr_(ip, c)) return true;
    }
    return false;
  }

  // ---- URL parsing + DNS ----
  static std::string extract_host_(const std::string& url) {
    // Very small URL parser:
    //   [scheme://]host[:port][/...]
    //   Supports IPv6 in brackets: http://[::1]:8080/ (host returned without brackets).
    std::string s = url;

    // strip scheme
    const auto scheme = s.find("://");
    std::size_t start = (scheme == std::string::npos) ? 0 : scheme + 3;

    // host ends at first of "/?#"
    const auto end = s.find_first_of("/?#", start);
    std::string hostport = s.substr(start, (end == std::string::npos) ? std::string::npos : (end - start));
    if (hostport.empty()) return "";

    // IPv6 bracket form
    if (!hostport.empty() && hostport[0] == '[') {
      const auto rb = hostport.find(']');
      if (rb == std::string::npos) return "";
      return hostport.substr(1, rb - 1);
    }

    // split host:port
    const auto colon = hostport.rfind(':');
    if (colon != std::string::npos) {
      // If there are multiple colons, it's probably an unbracketed IPv6 (unsupported here)
      if (hostport.find(':') != colon) return "";
      return hostport.substr(0, colon);
    }
    return hostport;
  }

  static bool host_to_ips_(const std::string& host, std::vector<IpAddr>& out) {
    out.clear();

    // If host is already an IP literal
    IpAddr ip;
    if (parse_ip(host, ip)) {
      out.push_back(ip);
      return true;
    }

    // DNS resolve hostname -> IPv4s and IPv6s
    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM; // doesn't matter; helps filtering

    addrinfo* res = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), nullptr, &hints, &res);
    if (rc != 0 || !res) return false;

    for (addrinfo* p = res; p; p = p->ai_next) {
      if (p->ai_family == AF_INET && p->ai_addr && p->ai_addrlen >= sizeof(sockaddr_in)) {
        const sockaddr_in* sin = reinterpret_cast<const sockaddr_in*>(p->ai_addr);
        IpAddr resolved;
        resolved.family = AF_INET;
        resolved.v4 = ntohl(sin->sin_addr.s_addr);
        out.push_back(resolved);
      } else if (p->ai_family == AF_INET6 && p->ai_addr && p->ai_addrlen >= sizeof(sockaddr_in6)) {
        const sockaddr_in6* sin6 = reinterpret_cast<const sockaddr_in6*>(p->ai_addr);
        IpAddr resolved;
        resolved.family = AF_INET6;
        for (size_t i = 0; i < resolved.v6.size(); i++) {
          resolved.v6[i] = sin6->sin6_addr.s6_addr[i];
        }
        out.push_back(resolved);
      }
    }
    ::freeaddrinfo(res);

    // de-dupe
    std::sort(out.begin(), out.end(), [](const IpAddr& a, const IpAddr& b) {
      if (a.family != b.family) return a.family < b.family;
      if (a.family == AF_INET) return a.v4 < b.v4;
      return a.v6 < b.v6;
    });
    out.erase(std::unique(out.begin(), out.end(), same_ip_), out.end());
    return !out.empty();
  }

private:
  // Class-level config
  std::mutex cfg_mu_;
  std::vector<Cidr> disallowed_dest_cidrs_;
  std::atomic<std::size_t> rate_limit_10s_;

  // Rate-limiter state
  std::mutex rate_mu_;
  std::deque<std::chrono::steady_clock::time_point> hits_;
};
