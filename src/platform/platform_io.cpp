#include <libspaznet/platform/platform_io.hpp>

#ifdef USE_EPOLL
#include "platform_io_epoll.cpp"
#elif defined(USE_KQUEUE)
#include "platform_io_kqueue.cpp"
#elif defined(USE_IOCP)
#include "platform_io_iocp.cpp"
#else
#include "platform_io_poll.cpp"
#endif

namespace spaznet {

auto create_platform_io() -> std::unique_ptr<PlatformIO> {
#ifdef USE_EPOLL
    return std::make_unique<PlatformIOEpoll>();
#elif defined(USE_KQUEUE)
    return std::make_unique<PlatformIOKqueue>();
#elif defined(USE_IOCP)
    return std::make_unique<PlatformIOIOCP>();
#else
    return std::make_unique<PlatformIOPoll>();
#endif
}

} // namespace spaznet
