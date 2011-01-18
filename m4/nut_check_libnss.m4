dnl Check for Mozilla NSS (LIBNSS) compiler flags. On success, set 
dnl nut_have_libnss="yes" and nut_ssl_lib="Mozilla NSS", and define WITH_SSL,
dnl WITH_NSS, LIBSSL_CFLAGS and LIBSSL_LDFLAGS. On failure, set nut_have_libnss="no".
dnl This macro can be run multiple times, but will do the checking only once. 

AC_DEFUN([NUT_CHECK_LIBNSS], 
[
if test -z "${nut_have_libnss_seen}"; then
	nut_have_libnss_seen=yes

	dnl save CFLAGS and LDFLAGS
	CFLAGS_ORIG="${CFLAGS}"
	LDFLAGS_ORIG="${LDFLAGS}"
	LIBS_ORIG="${LIBS}"

	AC_MSG_CHECKING(for Mozilla NSS version via pkg-config)
	NSS_VERSION="`pkg-config --silence-errors --modversion nss 2>/dev/null`"
	if test "$?" = "0" -a -n "${NSS_VERSION}"; then
		CFLAGS="`pkg-config --silence-errors --cflags nss 2>/dev/null`"
		LDFLAGS=""
		LIBS="`pkg-config --silence-errors --libs nss 2>/dev/null`"
	else
		NSS_VERSION="none"
		CFLAGS=""
		LDFLAGS=""
		LIBS="-lnss3 -lnssutil3 -lsmime3 -lssl3 -lplds4 -lplc4 -lnspr4"
	fi
	AC_MSG_RESULT(${NSS_VERSION} found)

	dnl allow overriding NSS settings if the user knows best
	AC_MSG_CHECKING(for Mozilla NSS cflags)
	AC_ARG_WITH(nss-includes,
		AS_HELP_STRING([@<:@--with-nss-includes=CFLAGS@:>@], [include flags for the Mozilla NSS library]),
	[
		case "${withval}" in
		yes|no)
			AC_MSG_ERROR(invalid option --with(out)-nss-includes - see docs/configure.txt)
			;;
		*)
			CFLAGS="${withval}"
			;;
		esac
	], [])
	AC_MSG_RESULT([${CFLAGS}])

	AC_MSG_CHECKING(for Mozilla NSS ldflags)
	AC_ARG_WITH(nss-libs,
		AS_HELP_STRING([@<:@--with-nss-libs=LDFLAGS@:>@], [linker flags for the Mozilla NSS library]),
	[
		case "${withval}" in
		yes|no)
			AC_MSG_ERROR(invalid option --with(out)-nss-libs - see docs/configure.txt)
			;;
		*)
			LDFLAGS="${withval}"
			;;
		esac
	], [])
	AC_MSG_RESULT([${LDFLAGS}])

	dnl check if NSS is usable
	AC_CHECK_HEADERS(nss.h, [nut_have_libnss=yes], [nut_have_libnss=no], [AC_INCLUDES_DEFAULT])
	AC_CHECK_FUNCS(NSS_Init, [], [nut_have_libnss=no])
	
	if test "${nut_have_libnss}" = "yes"; then
		nut_with_ssl="yes"
		nut_ssl_lib="(Mozilla NSS)"
		AC_DEFINE(WITH_SSL, 1, [Define to enable SSL support])
		AC_DEFINE(WITH_NSS, 1, [Define to enable SSL support using Mozilla NSS])
		LIBSSL_CFLAGS="${CFLAGS}"
		LIBSSL_LDFLAGS="${LDFLAGS} ${LIBS}"
	fi

	dnl restore original CFLAGS and LDFLAGS
	CFLAGS="${CFLAGS_ORIG}"
	LDFLAGS="${LDFLAGS_ORIG}"
	LIBS="${LIBS_ORIG}"
fi
])