AC_INIT([cpulock], [1.0], [cpulock@logos.t.u-tokyo.ac.jp], [cpulock], [git@github.com:taura/cpulock.git])
AM_INIT_AUTOMAKE([foreign -Wall])
AC_CONFIG_MACRO_DIR([m4])
AM_MAINTAINER_MODE([disable])
AC_PROG_CC
AC_CHECK_LIB([pthread],  [pthread_mutexattr_setpshared])
AC_CHECK_LIB([pthread],  [pthread_condattr_setpshared], [])
AC_CONFIG_FILES([Makefile src/Makefile])
AC_OUTPUT
