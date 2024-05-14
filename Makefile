all:
	mkdir build && cd build && cmake ../ && make all && mv ./multithreaded_crawler .. && cd .. && rm -rf build
clean:
	rm -rf findpng3
