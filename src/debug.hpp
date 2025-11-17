#ifndef _SFTP_NODE_DEBUG
#define _SFTP_NODE_DEBUG

#ifndef NDEBUG
#	include <cassert> // for assert()

/** Alias for SIGTRAP in debug mode. */
#	if defined(_MSC_VER)
#		define BREAKPOINT() __debugbreak()
#	elif defined(__has_builtin) && __has_builtin(__builtin_debugtrap)
#		define BREAKPOINT() __builtin_debugtrap()
#	else
#		include <signal.h>
#		define BREAKPOINT() raise(SIGTRAP)
#	endif

#	define LOG_DBG(...) fprintf(stderr, __VA_ARGS__)

#	define UNREACHABLE_MSG(...)                                               \
		do {                                                                   \
			LOG_DBG(__VA_ARGS__);                                              \
			fprintf(                                                           \
				stderr, "Should be Unreachable %s:%d\n", __FILE__, __LINE__);  \
			BREAKPOINT();                                                      \
		} while (0)

#else

#	define BREAKPOINT()         ((void)0)
#	define assert(...)          ((void)0)
#	define LOG_DBG(...)         ((void)0)
#	define UNREACHABLE_MSG(...) ((void)0)

#endif

#endif
