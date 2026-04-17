# Makefile for building the shard project

CXX = g++
CXXFLAGS = -std=c++17 -g -Wall
LDFLAGS = -pthread

# 目录结构
SRC_DIR = source
BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj
BIN_DIR = $(BUILD_DIR)/bin

# 自动查找所有源文件
SRCS = $(wildcard $(SRC_DIR)/*.cpp)
OBJS = $(patsubst $(SRC_DIR)/%.cpp, $(OBJ_DIR)/%.o, $(SRCS))
EXEC = $(BIN_DIR)/hierachain

# 默认目标
all: $(BUILD_DIR) $(OBJ_DIR) $(BIN_DIR) $(EXEC)

# 创建目录（作为订单依赖，不检查时间戳）
$(BUILD_DIR) $(OBJ_DIR) $(BIN_DIR):
	mkdir -p $@

# 链接
$(EXEC): $(OBJS) | $(BIN_DIR)
	$(CXX) $(OBJS) $(LDFLAGS) -o $@
	@echo "✓ Build success: $@"

# 编译（确保 OBJ_DIR 存在）
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# 清理
clean:
	rm -rf $(BUILD_DIR)
	@echo "✓ Cleaned: $(BUILD_DIR)"

# 运行
run: $(EXEC)
	./$(EXEC)

# 帮助
help:
	@echo "Available targets:"
	@echo "  make        - Build the project"
	@echo "  make clean  - Remove build directory"
	@echo "  make run    - Build and run the executable"

.PHONY: all clean run help

# # Makefile for building the shard project

# # Compiler and Linker
# CXX = g++
# CXXFLAGS = -std=c++11 -g -Wall
# LDFLAGS = -pthread

# # Directories (optional)
# SRC_DIR = ./source
# OBJ_DIR = ./source

# # Source files
# SRCS = $(SRC_DIR)/main.cpp $(SRC_DIR)/common.cpp $(SRC_DIR)/shard.cpp $(SRC_DIR)/shardsManager.cpp

# # Object files
# OBJS = $(OBJ_DIR)/main.o $(OBJ_DIR)/common.o $(OBJ_DIR)/shard.o $(OBJ_DIR)/shardsManager.o

# # Output executable
# EXEC = hierachain

# # Targets
# all: $(EXEC)

# # Link the object files into the final executable
# $(EXEC): $(OBJS)
# 	$(CXX) $(OBJS) $(LDFLAGS) -o $(EXEC)

# # Rule for compiling .cpp files into .o object files
# $(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp $(SRC_DIR)/shard.h
# 	$(CXX) $(CXXFLAGS) -c $< -o $@

# # Clean rule to remove compiled files
# clean:
# 	rm -f $(OBJ_DIR)/*.o $(EXEC)

# # Phony targets
# .PHONY: all clean
