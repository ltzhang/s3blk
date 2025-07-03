#!/bin/bash

# Test script for pageserver functionality

set -e

echo "Testing pageserver functionality..."

# Check if pageserver binary exists
if [ ! -f "./pageserver" ]; then
    echo "Error: pageserver binary not found. Please build the project first."
    exit 1
fi

# Function to kill any running pageserver processes
kill_pageserver() {
    pkill -f "./pageserver" 2>/dev/null || true
    sleep 1
}

# Function to check if port is free
check_port() {
    local port=$1
    if lsof -i :$port >/dev/null 2>&1; then
        echo "   Warning: Port $port is in use, killing processes..."
        kill_pageserver
        sleep 2
    fi
}

# Clean up any existing test files and processes
rm -f test_file_*.dat
kill_pageserver

echo "1. Testing creation of new file with size..."
check_port 8081
./pageserver -f test_file_new.dat -s 1M -v -p 8081 &
SERVER_PID=$!
sleep 2

# Check if file was created with correct size
if [ -f test_file_new.dat ]; then
    SIZE=$(stat -c%s test_file_new.dat)
    echo "   ✓ File created with size: $SIZE bytes"
    if [ $SIZE -eq 1048576 ]; then
        echo "   ✓ File size is correct (1MB)"
    else
        echo "   ✗ File size is incorrect: expected 1048576, got $SIZE"
        kill $SERVER_PID 2>/dev/null || true
        exit 1
    fi
else
    echo "   ✗ File was not created"
    kill $SERVER_PID 2>/dev/null || true
    exit 1
fi

kill $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true
kill_pageserver

echo "2. Testing with existing file (file_size should be set to actual size)..."
# Create a file manually with a different size
dd if=/dev/zero of=test_file_existing.dat bs=1M count=2 2>/dev/null
EXPECTED_SIZE=$(stat -c%s test_file_existing.dat)
echo "   Created file with size: $EXPECTED_SIZE bytes"

# Test the server output directly
echo "   Testing server output..."
check_port 8082
SERVER_OUTPUT=$(timeout 3s ./pageserver -f test_file_existing.dat -v -p 8082 2>&1)
GREP_PATTERN="size: $EXPECTED_SIZE bytes"
echo "   [DEBUG] Grep pattern: '$GREP_PATTERN'"
echo "   [DEBUG] Actual output: '$SERVER_OUTPUT'"
if echo "$SERVER_OUTPUT" | grep -Fq "$GREP_PATTERN"; then
    echo "   ✓ Server correctly detected file size: $EXPECTED_SIZE bytes"
else
    echo "   ✗ Server did not detect correct file size"
    echo "   Expected: $EXPECTED_SIZE bytes"
    echo "   Actual output: $SERVER_OUTPUT"
    exit 1
fi
kill_pageserver

echo "3. Testing error case: existing file with size specified..."
check_port 8083
if ./pageserver -f test_file_existing.dat -s 1M -v -p 8083 2>&1 | grep -q "already exists"; then
    echo "   ✓ Correctly rejected existing file with size"
else
    echo "   ✗ Should have rejected existing file with size"
    exit 1
fi
kill_pageserver

echo "4. Testing error case: non-existent file without size..."
check_port 8084
if ./pageserver -f test_file_nonexistent.dat -v -p 8084 2>&1 | grep -q "does not exist"; then
    echo "   ✓ Correctly rejected non-existent file without size"
else
    echo "   ✗ Should have rejected non-existent file without size"
    exit 1
fi
kill_pageserver

echo "5. Testing size parsing with suffixes..."
check_port 8085
./pageserver -f test_file_k.dat -s 1K -v -p 8085 &
SERVER_PID=$!
sleep 2
kill $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true

if [ -f test_file_k.dat ]; then
    SIZE=$(stat -c%s test_file_k.dat)
    if [ $SIZE -eq 1024 ]; then
        echo "   ✓ 1K suffix parsed correctly"
    else
        echo "   ✗ 1K suffix parsed incorrectly: expected 1024, got $SIZE"
        exit 1
    fi
fi
kill_pageserver

# Clean up
rm -f test_file_*.dat
kill_pageserver

echo "All tests passed! ✓" 