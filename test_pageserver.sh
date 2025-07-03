#!/bin/bash

# Test script for pageserver functionality

set -e

echo "Testing pageserver functionality..."

# Check if pageserver binary exists
if [ ! -f "./pageserver" ]; then
    echo "Error: pageserver binary not found. Please build the project first."
    exit 1
fi

# Clean up any existing test files
rm -f test_file_*.dat

echo "1. Testing creation of new file with size..."
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
        exit 1
    fi
else
    echo "   ✗ File was not created"
    exit 1
fi

kill $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true

echo "2. Testing with existing file (file_size should be set to actual size)..."
# Create a file manually with a different size
dd if=/dev/zero of=test_file_existing.dat bs=1M count=2 2>/dev/null
EXPECTED_SIZE=$(stat -c%s test_file_existing.dat)
echo "   Created file with size: $EXPECTED_SIZE bytes"

# Test the server output directly
echo "   Testing server output..."
if timeout 3s ./pageserver -f test_file_existing.dat -v -p 8082 2>&1 | grep -q "size: $EXPECTED_SIZE bytes"; then
    echo "   ✓ Server correctly detected file size: $EXPECTED_SIZE bytes"
else
    echo "   ✗ Server did not detect correct file size"
    echo "   Expected: $EXPECTED_SIZE bytes"
    echo "   Actual output:"
    timeout 3s ./pageserver -f test_file_existing.dat -v -p 8082 2>&1 || true
    exit 1
fi

echo "3. Testing error case: existing file with size specified..."
if ./pageserver -f test_file_existing.dat -s 1M -v -p 8083 2>&1 | grep -q "already exists"; then
    echo "   ✓ Correctly rejected existing file with size"
else
    echo "   ✗ Should have rejected existing file with size"
    exit 1
fi

echo "4. Testing error case: non-existent file without size..."
if ./pageserver -f test_file_nonexistent.dat -v -p 8084 2>&1 | grep -q "does not exist"; then
    echo "   ✓ Correctly rejected non-existent file without size"
else
    echo "   ✗ Should have rejected non-existent file without size"
    exit 1
fi

echo "5. Testing size parsing with suffixes..."
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

# Clean up
rm -f test_file_*.dat

echo "All tests passed! ✓" 