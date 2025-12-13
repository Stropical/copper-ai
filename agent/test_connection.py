#!/usr/bin/env python3
"""
Simple test script to verify the Python agent is working correctly.
"""

import requests
import json
import sys

AGENT_URL = "http://127.0.0.1:5001"

def test_health():
    """Test the health endpoint."""
    print("Testing /health endpoint...")
    try:
        response = requests.get(f"{AGENT_URL}/health", timeout=5)
        print(f"  Status: {response.status_code}")
        print(f"  Response: {json.dumps(response.json(), indent=2)}")
        return response.status_code == 200
    except Exception as e:
        print(f"  ERROR: {e}")
        return False

def test_tags():
    """Test the /api/tags endpoint."""
    print("\nTesting /api/tags endpoint...")
    try:
        response = requests.get(f"{AGENT_URL}/api/tags", timeout=5)
        print(f"  Status: {response.status_code}")
        if response.status_code == 200:
            data = response.json()
            models = data.get("models", [])
            print(f"  Found {len(models)} models")
            if models:
                print(f"  First model: {models[0].get('name', 'unknown')}")
        return response.status_code == 200
    except Exception as e:
        print(f"  ERROR: {e}")
        return False

def test_generate():
    """Test a simple generation request."""
    print("\nTesting /api/generate endpoint (non-streaming)...")
    try:
        payload = {
            "model": "qwen3:4b",
            "prompt": "Say hello",
            "stream": False
        }
        response = requests.post(f"{AGENT_URL}/api/generate", json=payload, timeout=30)
        print(f"  Status: {response.status_code}")
        if response.status_code == 200:
            data = response.json()
            if "response" in data:
                response_text = data["response"][:100]
                print(f"  Response preview: {response_text}...")
                return True
            else:
                print(f"  Unexpected response format: {list(data.keys())}")
                return False
        else:
            print(f"  Error response: {response.text[:200]}")
            return False
    except Exception as e:
        print(f"  ERROR: {e}")
        return False

if __name__ == "__main__":
    print("=" * 60)
    print("Testing Python Agent Connection")
    print("=" * 60)
    
    results = []
    results.append(("Health Check", test_health()))
    results.append(("Tags Endpoint", test_tags()))
    results.append(("Generate Endpoint", test_generate()))
    
    print("\n" + "=" * 60)
    print("Test Results:")
    print("=" * 60)
    for name, result in results:
        status = "✓ PASS" if result else "✗ FAIL"
        print(f"  {status}: {name}")
    
    all_passed = all(result for _, result in results)
    print("=" * 60)
    if all_passed:
        print("All tests passed! Agent is working correctly.")
        sys.exit(0)
    else:
        print("Some tests failed. Check the agent logs for details.")
        sys.exit(1)

