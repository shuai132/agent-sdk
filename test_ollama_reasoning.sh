#!/bin/bash

# Test script for Ollama reasoning/thinking functionality

export OLLAMA_API_KEY=""

# Test with different Ollama models
models=("qwen3:0.6b" "deepseek-r1:1.5b" "deepseek-r1:7b")

echo "Testing Ollama reasoning functionality..."
echo "Available models to test: ${models[@]}"

for model in "${models[@]}"; do
    echo "----------------------------------------"
    echo "Testing with model: $model"
    echo "----------------------------------------"
    
    export OLLAMA_MODEL="$model"
    
    # Test with a math problem that requires reasoning
    echo "Solving: What is 15 * 23 + 7 - 12?"
    echo "Expected to see thinking process..."
    
    # Use timeout to prevent hanging
    echo "What is 15 * 23 + 7 - 12? Show your reasoning step by step." | timeout 30 ./build/agent_cli
    
    if [ $? -eq 124 ]; then
        echo "Test timed out for model $model"
    else
        echo "Test completed for model $model"
    fi
    
    echo ""
    sleep 2
done

echo "All tests completed!"