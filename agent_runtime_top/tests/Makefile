# Agent Runtime Makefile
# 用法：
#   make test-multi-agent - 编译多Agent多轮对话测试（需要llama.cpp）
#   make clean           - 清理

CC = gcc
CFLAGS = -Wall -Wextra -O2 -I../include
LDFLAGS = -lpthread

# llama.cpp路径（根据实际安装位置修改）
LLAMA_DIR ?= ~/llama.cpp-master
LLAMA_CFLAGS = -I$(LLAMA_DIR)/include -I$(LLAMA_DIR)/ggml/include
LLAMA_LDFLAGS = -L$(LLAMA_DIR)/build/bin -lllama -lggml

SRC_DIR = ../src
BUILD_DIR = build

# 核心源文件
CORE_SRCS = $(SRC_DIR)/agent.c $(SRC_DIR)/scheduler.c \
            $(SRC_DIR)/kv_cache.c $(SRC_DIR)/resource.c \
            $(SRC_DIR)/llm_llamacpp.c

.PHONY: all clean

# 多Agent多轮对话测试（需要llama.cpp库）
all: $(BUILD_DIR) test_multi_agent

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

test_multi_agent: $(BUILD_DIR)/agent.o $(BUILD_DIR)/scheduler.o \
                  $(BUILD_DIR)/kv_cache.o $(BUILD_DIR)/resource.o \
                  $(BUILD_DIR)/llm_llamacpp.o $(BUILD_DIR)/test_multi_agent.o
	$(CC) -o $@ $^ $(LDFLAGS) $(LLAMA_LDFLAGS)

# 通用编译规则
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# 带llama.cpp的编译规则
$(BUILD_DIR)/llm_llamacpp.o: $(SRC_DIR)/llm_llamacpp.c
	$(CC) $(CFLAGS) $(LLAMA_CFLAGS) -DUSE_LLAMA_CPP -c -o $@ $<

$(BUILD_DIR)/test_multi_agent.o: test_multi_agent.c
	$(CC) $(CFLAGS) $(LLAMA_CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD_DIR) test_multi_agent
