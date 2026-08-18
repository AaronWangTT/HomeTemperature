import argparse
import urllib.request
import urllib.error
import socket
import json
import sys

parser = argparse.ArgumentParser(description="Smoke-test the AZ3166 local HTTP endpoint")
parser.add_argument("target_ip", help="AZ3166 IPv4 address or hostname")
target_ip = parser.parse_args().target_ip
all_tests_passed = True

print("--- Test 1: GET http://{}/api/telemetry ---".format(target_ip))
try:
    req = urllib.request.Request("http://{}/api/telemetry".format(target_ip))
    with urllib.request.urlopen(req, timeout=15) as response:
        status_code = response.getcode()
        content_type = response.headers.get("Content-Type", "")
        body = response.read().decode("utf-8")
        
        print("Status Code: {}".format(status_code))
        print("Content-Type: {}".format(content_type))
        print("Body: {}".format(body))
        
        v1 = True
        if status_code != 200:
            print("[FAIL] Status code is not 200")
            v1 = False
        if "application/json" not in content_type:
            print("[FAIL] Content-Type does not contain application/json")
            v1 = False
            
        try:
            data = json.loads(body)
            device_id = data.get("deviceId", "")
            if not device_id.startswith("az3166-"):
                print("[FAIL] deviceId '{}' does not start with az3166-".format(device_id))
                v1 = False
            
            temp = data.get("temperature")
            hum = data.get("humidity")
            press = data.get("pressure")
            
            if temp is None or not isinstance(temp, (int, float)):
                print("[FAIL] temperature is not a number/is missing: {}".format(temp))
                v1 = False
            if hum is None or not isinstance(hum, (int, float)):
                print("[FAIL] humidity is not a number/is missing: {}".format(hum))
                v1 = False
            if press is None or not isinstance(press, (int, float)):
                print("[FAIL] pressure is not a number/is missing: {}".format(press))
                v1 = False
                
        except Exception as e:
            print("[FAIL] Could not parse body as JSON: {}".format(e))
            v1 = False
            
        if v1:
            print("[PASS] Test 1 succeeded")
        else:
            print("[FAIL] Test 1 failed validations")
            all_tests_passed = False
except Exception as e:
    print("[FAIL] Test 1 Error: {}".format(e))
    all_tests_passed = False

print("\n--- Test 2: GET http://{}/api/unknown ---".format(target_ip))
try:
    req2 = urllib.request.Request("http://{}/api/unknown".format(target_ip))
    with urllib.request.urlopen(req2, timeout=15) as r:
        status_code2 = r.getcode()
        content_type2 = r.headers.get("Content-Type", "")
        body2 = r.read().decode("utf-8")
except urllib.error.HTTPError as e:
    status_code2 = e.code
    content_type2 = e.headers.get("Content-Type", "")
    body2 = e.read().decode("utf-8")
except Exception as e:
    status_code2 = None
    content_type2 = ""
    body2 = str(e)

if status_code2 is not None:
    print("Status Code: {}".format(status_code2))
    print("Content-Type: {}".format(content_type2))
    print("Body: {!r}".format(body2))
    
    v2 = True
    if status_code2 != 404:
        print("[FAIL] Status is not 404")
        v2 = False
    if "application/json" not in content_type2:
        print("[FAIL] Content-Type does not contain application/json")
        v2 = False
        
    trimmed_body = body2.strip()
    expected_body = '{"error":"not found"}'
    if trimmed_body != expected_body:
        print("[FAIL] Body is not exact. Expected: {!r}, Got: {!r}".format(expected_body, trimmed_body))
        v2 = False
        
    if v2:
        print("[PASS] Test 2 succeeded")
    else:
        print("[FAIL] Test 2 failed validations")
        all_tests_passed = False
else:
    print("[FAIL] No/invalid response received for Test 2: {}".format(body2))
    all_tests_passed = False

print("\n--- Test 3: Raw TCP connection to {}:80 ---".format(target_ip))
try:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5.0)
    s.connect((target_ip, 80))
    s.sendall(b"\r\n\r\n")
    
    response_bytes = b""
    while True:
        try:
            chunk = s.recv(4096)
            if not chunk:
                break
            response_bytes += chunk
        except socket.timeout:
            print("Socket read timed out")
            break
        except Exception as e:
            print("Exception reading socket: {}".format(e))
            break
            
    s.close()
    
    raw_response = response_bytes.decode("ascii", errors="replace")
    print("Raw Response:\n{}".format(raw_response))
    
    v3 = True
    if not raw_response.startswith("HTTP/1.1 400 Bad Request"):
        print("[FAIL] Status line is not HTTP/1.1 400 Bad Request")
        v3 = False
    if "Content-Type:" not in raw_response:
        print("[FAIL] Content-Type header missing")
        v3 = False
    elif "application/json" not in raw_response.split("Content-Type:")[1].split("\r\n")[0]:
        print("[FAIL] Content-Type is not application/json")
        v3 = False
        
    parts = raw_response.split("\r\n\r\n")
    if len(parts) >= 2:
        body3 = parts[1].strip()
        expected_body3 = '{"error":"bad request"}'
        if body3 != expected_body3:
            print("[FAIL] Body of raw TCP response is not exact. Expected: {!r}, Got: {!r}".format(expected_body3, body3))
            v3 = False
    else:
        print("[FAIL] Response header and body are not separated by \\r\\n\\r\\n")
        v3 = False
        
    if v3:
        print("[PASS] Test 3 succeeded")
    else:
        print("[FAIL] Test 3 failed validations")
        all_tests_passed = False
except Exception as e:
    print("[FAIL] Test 3 Error: {}".format(e))
    all_tests_passed = False

sys.exit(0 if all_tests_passed else 1)
