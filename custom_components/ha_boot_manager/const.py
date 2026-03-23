"""Constants for HA Boot Manager integration."""

DOMAIN = "ha_boot_manager"

CONF_TCP_PORT = "tcp_port"
CONF_DEFAULT_OS = "default_os"

DEFAULT_TCP_PORT = 9999
DEFAULT_DEFAULT_OS = "Menu"

ATTR_LAST_BOOT_TIME = "last_boot_time"
ATTR_LAST_CLIENT_IP = "last_client_ip"

EVENT_BOOT_REQUEST = f"{DOMAIN}_boot_request"

SIGNAL_OS_LIST_UPDATE = f"{DOMAIN}_os_list_update"
SIGNAL_CONNECTION = f"{DOMAIN}_connection"
