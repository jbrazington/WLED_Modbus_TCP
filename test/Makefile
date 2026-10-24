# Host-side unit tests: compiles the usermod against a minimal WLED stub (test/wled.h)
# and exercises the Modbus frame handling. Run: make -C test
WLED_TAG ?= v16.0.1
JSON_URL = https://raw.githubusercontent.com/wled/WLED/$(WLED_TAG)/wled00/src/dependencies/json/ArduinoJson-v6.h

test: modbus_test
	./modbus_test

modbus_test: test.cpp wled.h ../usermods/Modbus_TCP/Modbus_TCP.cpp ArduinoJson-v6.h
	$(CXX) -std=gnu++17 -I. -Wall -Wno-unused -o $@ test.cpp

ArduinoJson-v6.h:
	curl -sSfL -o $@ $(JSON_URL)

clean:
	rm -f modbus_test ArduinoJson-v6.h

.PHONY: test clean
