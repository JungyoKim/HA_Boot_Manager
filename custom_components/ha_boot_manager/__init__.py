"""HA Boot Manager - Custom Integration for UEFI Boot Manager."""

from __future__ import annotations

import asyncio
import logging

import voluptuous as vol

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, ServiceCall
from homeassistant.helpers.dispatcher import async_dispatcher_send
from homeassistant.util import dt as dt_util

from .const import (
    DOMAIN,
    CONF_TCP_PORT,
    CONF_DEFAULT_OS,
    DEFAULT_TCP_PORT,
    DEFAULT_DEFAULT_OS,
    EVENT_BOOT_REQUEST,
    SIGNAL_OS_LIST_UPDATE,
    SIGNAL_CONNECTION,
)

_LOGGER = logging.getLogger(__name__)

PLATFORMS = ["select", "sensor"]


class BootManagerTcpServer:
    """Async TCP server for UEFI boot manager communication."""

    def __init__(self, hass: HomeAssistant, port: int, default_os: str) -> None:
        """Initialize the TCP server."""
        self.hass = hass
        self.port = port
        self.default_os = default_os
        self._server: asyncio.Server | None = None

    async def start(self) -> None:
        """Start the TCP server."""
        self._server = await asyncio.start_server(
            self._handle_client, "0.0.0.0", self.port
        )
        _LOGGER.info("Boot Manager TCP server started on port %d", self.port)

    async def stop(self) -> None:
        """Stop the TCP server."""
        if self._server:
            self._server.close()
            await self._server.wait_closed()
            self._server = None
            _LOGGER.info("Boot Manager TCP server stopped")

    async def _handle_client(
        self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter
    ) -> None:
        """Handle a single UEFI client connection."""
        addr = writer.get_extra_info("peername")
        client_ip = addr[0] if addr else "unknown"
        _LOGGER.info("Boot manager connection from %s", client_ip)

        try:
            data = await asyncio.wait_for(reader.read(512), timeout=10.0)
            os_list_str = data.decode("utf-8").strip()

            if os_list_str:
                os_list = [
                    os_name.strip()
                    for os_name in os_list_str.split(",")
                    if os_name.strip()
                ]
                _LOGGER.info("Received OS list: %s", os_list)

                # Update select entity options
                async_dispatcher_send(self.hass, SIGNAL_OS_LIST_UPDATE, os_list)

                # Fire event
                self.hass.bus.async_fire(
                    EVENT_BOOT_REQUEST,
                    {"os_list": os_list, "client_ip": client_ip},
                )

                # Update status sensor
                async_dispatcher_send(
                    self.hass,
                    SIGNAL_CONNECTION,
                    {
                        "client_ip": client_ip,
                        "timestamp": dt_util.utcnow().isoformat(),
                    },
                )

            # Read current selection from the select entity
            select_state = self.hass.states.get(f"select.{DOMAIN}_boot_os")
            if select_state and select_state.state not in ("unknown", "unavailable"):
                response = select_state.state
            else:
                response = self.default_os

            _LOGGER.info("Sending boot selection: %s", response)
            writer.write(response.encode("utf-8"))
            await writer.drain()

            # Reset select entity to "Menu" after responding
            async def _reset_to_menu():
                await asyncio.sleep(1)
                select_entity = self.hass.data.get(DOMAIN, {}).get("select_entity")
                if select_entity:
                    if "Menu" in select_entity.options:
                        select_entity._attr_current_option = "Menu"
                        select_entity.async_write_ha_state()
                        _LOGGER.info("Reset boot selection to Menu")
                    else:
                        _LOGGER.warning("Menu not in options: %s", select_entity.options)

            self.hass.async_create_task(_reset_to_menu())

        except asyncio.TimeoutError:
            _LOGGER.warning("Client %s timed out", client_ip)
            writer.write(self.default_os.encode("utf-8"))
            await writer.drain()
        except Exception:
            _LOGGER.exception("Error handling boot manager connection from %s", client_ip)
            writer.write(self.default_os.encode("utf-8"))
            await writer.drain()
        finally:
            writer.close()
            await writer.wait_closed()


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Set up HA Boot Manager from a config entry."""
    port = entry.data.get(CONF_TCP_PORT, DEFAULT_TCP_PORT)
    default_os = entry.data.get(CONF_DEFAULT_OS, DEFAULT_DEFAULT_OS)

    tcp_server = BootManagerTcpServer(hass, port, default_os)

    hass.data.setdefault(DOMAIN, {})
    hass.data[DOMAIN] = {
        "tcp_server": tcp_server,
        "entry_id": entry.entry_id,
    }

    # Forward setup to platforms
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)

    # Start TCP server
    await tcp_server.start()

    # Register service
    async def handle_set_boot_os(call: ServiceCall) -> None:
        """Handle set_boot_os service call."""
        os_name = call.data["os_name"]
        select_entity = hass.data[DOMAIN].get("select_entity")
        if select_entity:
            await select_entity.async_select_option(os_name)

    if not hass.services.has_service(DOMAIN, "set_boot_os"):
        hass.services.async_register(
            DOMAIN,
            "set_boot_os",
            handle_set_boot_os,
            schema=vol.Schema({vol.Required("os_name"): str}),
        )

    # Listen for options updates
    entry.async_on_unload(entry.add_update_listener(_async_update_listener))

    return True


async def _async_update_listener(hass: HomeAssistant, entry: ConfigEntry) -> None:
    """Handle options update."""
    await hass.config_entries.async_reload(entry.entry_id)


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Unload HA Boot Manager config entry."""
    # Stop TCP server
    tcp_server = hass.data[DOMAIN].get("tcp_server")
    if tcp_server:
        await tcp_server.stop()

    # Unload platforms
    unload_ok = await hass.config_entries.async_unload_platforms(entry, PLATFORMS)

    # Remove service
    if hass.services.has_service(DOMAIN, "set_boot_os"):
        hass.services.async_remove(DOMAIN, "set_boot_os")

    # Clean up
    hass.data.pop(DOMAIN, None)

    return unload_ok
