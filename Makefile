.PHONY: all
all: echo-server test_speedometer

echo-server: echo-server.cpp echo-server.cflags echo-server.lflags
	$(CXX) $(file < echo-server.cflags) -o $@ $< $(file < echo-server.lflags)

test_speedometer: test_speedometer.cpp speedometer.h echo-server.cflags echo-server.lflags
	$(CXX) $(file < echo-server.cflags) -o $@ $< $(file < echo-server.lflags)

.PHONY: format
format:
	clang-format -i --style='{BasedOnStyle: Google, Language: Cpp, ColumnLimit: 80}' *.cpp *.h
