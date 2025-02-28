.PHONY: all
all: echo-server echo-server-simple forky

echo-server: echo-server.cpp echo-server.cflags echo-server.lflags
	$(CXX) $(file < echo-server.cflags) -o $@ $< $(file < echo-server.lflags)

echo-server-simple: echo-server-simple.cpp echo-server.cflags echo-server.lflags
	$(CXX) $(file < echo-server.cflags) -o $@ $< $(file < echo-server.lflags)

forky: forky.cpp echo-server.cflags echo-server.lflags
	$(CXX) $(file < echo-server.cflags) -o $@ $< $(file < echo-server.lflags)

.PHONY: format
format:
	find . -type f \( -name '*.h' -o -name '*.cpp' \) -print0 | xargs -0 clang-format -i --style='{BasedOnStyle: Google, Language: Cpp, ColumnLimit: 80}'
