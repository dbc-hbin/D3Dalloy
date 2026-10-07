#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { printf("DNS_REGRESSION_FAIL line=%d wsa=%d\n", __LINE__, WSAGetLastError()); return 1; } } while (0)

int main(void)
{
    WSADATA data;
    char hostname[256], host[256], service[32];
    struct in_addr loopback;
    struct hostent *entry;
    struct addrinfo hints = {0}, *addresses, *address;
    int count = 0;
    CHECK(!WSAStartup(MAKEWORD(2, 2), &data));
    CHECK(!gethostname(hostname, sizeof(hostname)) && hostname[0]);
    entry = gethostbyname("localhost");
    CHECK(entry && entry->h_name && entry->h_name[0] && entry->h_aliases && entry->h_addr_list && entry->h_addr_list[0]);
    CHECK(entry->h_addrtype == AF_INET && entry->h_length == sizeof(loopback));
    memcpy(&loopback, entry->h_addr_list[0], sizeof(loopback));
    entry = gethostbyaddr((const char *)&loopback, sizeof(loopback), AF_INET);
    CHECK(entry && entry->h_name && entry->h_name[0] && entry->h_addr_list && entry->h_addr_list[0]);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_CANONNAME;
    CHECK(!getaddrinfo("localhost", "80", &hints, &addresses));
    CHECK(addresses && addresses->ai_canonname && addresses->ai_canonname[0]);
    for (address = addresses; address; address = address->ai_next) {
        CHECK(address->ai_addr && address->ai_addrlen == sizeof(struct sockaddr_in));
        CHECK(!getnameinfo(address->ai_addr, (socklen_t)address->ai_addrlen, host, sizeof(host), service, sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV));
        CHECK(host[0] && !strcmp(service, "80"));
        ++count;
        CHECK(count < 32);
    }
    freeaddrinfo(addresses);
    CHECK(!getaddrinfo("127.0.0.1", "80", NULL, &addresses));
    CHECK(addresses && addresses->ai_addr);
    freeaddrinfo(addresses);
    hints.ai_flags = 0;
    CHECK(!getaddrinfo(NULL, "80", &hints, &addresses));
    CHECK(addresses && addresses->ai_addr);
    freeaddrinfo(addresses);
    CHECK(getaddrinfo(NULL, NULL, NULL, &addresses) == WSAHOST_NOT_FOUND);
    WSACleanup();
    printf("DNS_REGRESSION_PASS entries=%d\n", count);
    return 0;
}
