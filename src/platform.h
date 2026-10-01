/*
 * platform.h - Platform abstraction for cross-compilation
 *
 * Allows building/testing on Linux while targeting Windows.
 * On Linux: uses POSIX APIs for sockets, stubs WinUSB types.
 * On Windows: uses Win32, WinSock2, WinUSB natively.
 */
#ifndef PLATFORM_H
#define PLATFORM_H

#ifdef _WIN32
  /* ---- Windows ---- */
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <winusb.h>
  #include <setupapi.h>
  #include <initguid.h>

  #ifdef _MSC_VER
  #pragma comment(lib, "winusb.lib")
  #pragma comment(lib, "setupapi.lib")
  #pragma comment(lib, "ws2_32.lib")
  #pragma comment(lib, "advapi32.lib")
  #endif

  typedef SOCKET socket_t;
  #define INVALID_SOCK INVALID_SOCKET
  #define close_socket closesocket
  #define sock_errno   WSAGetLastError()

  static inline void platform_sleep_ms(int ms) { Sleep(ms); }

  static inline int platform_net_init(void) {
      WSADATA wsa;
      return WSAStartup(MAKEWORD(2, 2), &wsa);
  }
  static inline void platform_net_cleanup(void) { WSACleanup(); }

#else
  /* ---- Linux / POSIX (for testing & cross-compilation) ---- */
  #include <unistd.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <errno.h>
  #include <string.h>
  #include <time.h>

  /* Stub Windows types for compilation */
  typedef int            BOOL;
  typedef unsigned char  BYTE;
  typedef unsigned short USHORT;
  typedef unsigned long  ULONG;
  typedef unsigned long  DWORD;
  typedef void*          HANDLE;
  typedef void*          WINUSB_INTERFACE_HANDLE;
  typedef unsigned char  UCHAR;
  #define TRUE  1
  #define FALSE 0
  #define INVALID_HANDLE_VALUE ((HANDLE)(long)-1)

  typedef int socket_t;
  #define INVALID_SOCK (-1)
  #define close_socket close
  #define sock_errno   errno

  static inline void platform_sleep_ms(int ms) {
      struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
      nanosleep(&ts, NULL);
  }

  static inline int  platform_net_init(void)    { return 0; }
  static inline void platform_net_cleanup(void) {}

#endif /* _WIN32 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#endif /* PLATFORM_H */
