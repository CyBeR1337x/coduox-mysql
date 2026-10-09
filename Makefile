# Builds mysql.so, the MySQL plugin for coduox servers.
#
#   make                          build mysql.so
#   make test                     build and run the tests against a database
#   make install PLUGIN_DIR=<dir> copy mysql.so into a server's plugins folder
#   make clean
#
# The MariaDB or MySQL client development package must be installed, for
# example libmariadb-dev or default-libmysqlclient-dev on Debian and Ubuntu.
# To use a client library elsewhere, set MYSQL_CFLAGS and MYSQL_LIBS.
#
# include/coduomp_plugin.h is the server's plugin interface. To build against
# a newer copy from the server source, set CODUOMP_SDK=<coduox>/src/plugin.

CODUOMP_SDK ?= include

MYSQL_CONFIG ?= $(firstword $(shell command -v mariadb_config mysql_config 2>/dev/null))
ifneq ($(MYSQL_CONFIG),)
MYSQL_CFLAGS ?= $(shell $(MYSQL_CONFIG) --include)
MYSQL_LIBS ?= $(shell $(MYSQL_CONFIG) --libs)
else
MYSQL_CFLAGS ?= -I/usr/include/mariadb
MYSQL_LIBS ?= -lmariadb
endif

CXX ?= g++
CXXFLAGS ?= -O2
# Required regardless of what CXXFLAGS the caller passes.
PLUGIN_CXXFLAGS := -std=c++11 -Wall -Wextra -fPIC -fvisibility=hidden
TEST_CXXFLAGS := -std=c++11 -Wall -Wextra

TARGET := mysql.so
TEST := tests/test_plugin

.PHONY: all test install clean

all: $(TARGET)

$(TARGET): mysql_plugin.cpp $(CODUOMP_SDK)/coduomp_plugin.h
	$(CXX) $(PLUGIN_CXXFLAGS) $(CXXFLAGS) -I$(CODUOMP_SDK) $(MYSQL_CFLAGS) -shared \
		mysql_plugin.cpp -o $@ $(MYSQL_LIBS)

$(TEST): tests/test_plugin.cpp $(CODUOMP_SDK)/coduomp_plugin.h
	$(CXX) $(TEST_CXXFLAGS) $(CXXFLAGS) -I$(CODUOMP_SDK) tests/test_plugin.cpp -o $@ -ldl

# Needs a running database; see the top of tests/test_plugin.cpp.
test: $(TARGET) $(TEST)
	./$(TEST) ./$(TARGET)

install: $(TARGET)
	@test -n "$(PLUGIN_DIR)" || { echo "set PLUGIN_DIR=<server plugins folder>"; exit 1; }
	mkdir -p "$(PLUGIN_DIR)"
	cp $(TARGET) "$(PLUGIN_DIR)/$(TARGET)"

clean:
	rm -f $(TARGET) $(TEST)
