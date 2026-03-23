#!/usr/bin/env python3
"""
TCP Boot Selection Server v2.0 - Dynamic OS Discovery

1. Receives OS list from UEFI boot manager
2. Updates Home Assistant input_select with discovered OS options
3. Returns user-selected OS to boot manager

사용법:
  python3 boot_tcp_server.py

테스트:
  echo "Windows,Ubuntu,Menu" | nc localhost 9999
"""

import socket
import json
import urllib.request
import urllib.error

# Configuration
TCP_PORT = 9999
HA_URL = "http://192.168.0.67:8123"
HA_TOKEN = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJmMzUzNjI2NjVhYTE0NTM5OTljMGY5M2YzOGRiZTYxOCIsImlhdCI6MTc2NzMzNTYzNiwiZXhwIjoyMDgyNjk1NjM2fQ.ky_hGIUXoUgf5ftWXq2FlEgVjiQRjqEVdiXXZwyLAlU"
HA_ENTITY = "input_select.boot_os"


def get_boot_selection():
    """Home Assistant에서 현재 부팅 OS 선택 가져오기 (urllib 사용)"""
    url = f"{HA_URL}/api/states/{HA_ENTITY}"
    headers = {
        "Authorization": f"Bearer {HA_TOKEN}",
        "Content-Type": "application/json"
    }
    
    try:
        req = urllib.request.Request(url, headers=headers)
        with urllib.request.urlopen(req, timeout=5) as response:
            if response.status == 200:
                data = json.loads(response.read().decode('utf-8'))
                state = data.get("state", "ubuntu")
                return state.lower()
            else:
                print(f"HA API error: {response.status}")
                return "ubuntu"
    except urllib.error.URLError as e:
        print(f"Network error querying HA: {e}")
        return "ubuntu"
    except Exception as e:
        print(f"Error querying HA: {e}")
        return "ubuntu"


def update_input_select_options(os_list):
    """Home Assistant input_select 옵션 업데이트"""
    url = f"{HA_URL}/api/services/input_select/set_options"
    headers = {
        "Authorization": f"Bearer {HA_TOKEN}",
        "Content-Type": "application/json"
    }
    
    # Clean up OS names
    options = [os.strip() for os in os_list if os.strip()]
    if not options:
        print("No valid OS options received")
        return False
    
    data = json.dumps({
        "entity_id": HA_ENTITY,
        "options": options
    }).encode('utf-8')
    
    try:
        req = urllib.request.Request(url, data=data, headers=headers, method='POST')
        with urllib.request.urlopen(req, timeout=5) as response:
            if response.status == 200:
                print(f"Updated input_select options: {options}")
                return True
            else:
                print(f"Failed to update options: {response.status}")
                return False
    except Exception as e:
        print(f"Error updating input_select: {e}")
        return False


def start_server():
    """TCP 서버 시작"""
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    
    try:
        server.bind(('0.0.0.0', TCP_PORT))
        server.listen(5)
        print(f"Boot TCP Server v2.0 listening on port {TCP_PORT}")
        
        while True:
            conn, addr = server.accept()
            print(f"Connection from {addr}")
            
            try:
                # Receive OS list from UEFI
                data = conn.recv(512).decode('utf-8').strip()
                print(f"Received OS list: {data}")
                
                if data:
                    # Parse OS list (comma-separated)
                    os_list = data.split(',')
                    
                    # Update HA input_select with discovered OS options
                    update_input_select_options(os_list)
                
                # Get current selection from HA
                boot_os = get_boot_selection()
                print(f"Sending selection: {boot_os}")
                
                # Send selection back to UEFI
                conn.send(boot_os.encode('utf-8'))
                
            except Exception as e:
                print(f"Error handling connection: {e}")
                conn.send(b"menu")
            finally:
                conn.close()
                
    except KeyboardInterrupt:
        print("\nShutting down...")
    finally:
        server.close()


if __name__ == "__main__":
    start_server()
