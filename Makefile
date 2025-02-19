.PHONY: all
all: echo-server test_speedometer forky

echo-server: echo-server.cpp speedometer.h echo-server.cflags echo-server.lflags
	$(CXX) $(file < echo-server.cflags) -o $@ $< $(file < echo-server.lflags)

test_speedometer: test_speedometer.cpp speedometer.h echo-server.cflags echo-server.lflags
	$(CXX) $(file < echo-server.cflags) -o $@ $< $(file < echo-server.lflags)

forky: forky.cpp echo-server.cflags echo-server.lflags
	$(CXX) $(file < echo-server.cflags) -o $@ $< $(file < echo-server.lflags)

.PHONY: format
format:
	find . -type f \( -name '*.h' -o -name '*.cpp' \) -print0 | xargs -0 clang-format -i --style='{BasedOnStyle: Google, Language: Cpp, ColumnLimit: 80}'
