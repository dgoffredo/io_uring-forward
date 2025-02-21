.PHONY: all
all: echo-server forky

echo-server: echo-server.cpp echo-server.cflags echo-server.lflags # .last-format
	$(CXX) $(file < echo-server.cflags) -o $@ $< $(file < echo-server.lflags)

forky: forky.cpp echo-server.cflags echo-server.lflags # .last-format
	$(CXX) $(file < echo-server.cflags) -o $@ $< $(file < echo-server.lflags)

.PHONY: format
format: .last-format

.last-format: *.cpp
	find . -type f \( -name '*.h' -o -name '*.cpp' \) -print0 | xargs -0 clang-format -i --style='{BasedOnStyle: Google, Language: Cpp, ColumnLimit: 80}'
	touch $@
