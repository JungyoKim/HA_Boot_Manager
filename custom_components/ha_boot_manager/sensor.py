"""Sensor entity for HA Boot Manager - connection status."""

from __future__ import annotations

from homeassistant.components.sensor import SensorEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.dispatcher import async_dispatcher_connect
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.event import async_call_later

from .const import DOMAIN, ATTR_LAST_BOOT_TIME, ATTR_LAST_CLIENT_IP, SIGNAL_CONNECTION


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities: AddEntitiesCallback,
) -> None:
    """Set up the Boot Manager status sensor."""
    async_add_entities([BootManagerStatusSensor(entry)])


class BootManagerStatusSensor(SensorEntity):
    """Sensor showing boot manager connection status."""

    _attr_has_entity_name = True
    _attr_name = "Status"
    _attr_icon = "mdi:server-network"

    def __init__(self, entry: ConfigEntry) -> None:
        """Initialize the sensor."""
        self._entry = entry
        self._attr_unique_id = f"{DOMAIN}_status"
        self._attr_native_value = "offline"
        self._attr_extra_state_attributes = {
            ATTR_LAST_BOOT_TIME: None,
            ATTR_LAST_CLIENT_IP: None,
        }
        self._offline_cancel = None

    @property
    def device_info(self):
        """Return device info."""
        return {
            "identifiers": {(DOMAIN, self._entry.entry_id)},
            "name": "HA Boot Manager",
            "manufacturer": "HaBootManager",
            "model": "UEFI Boot Manager",
            "sw_version": "1.0.0",
        }

    async def async_added_to_hass(self) -> None:
        """Subscribe to connection events."""
        self.async_on_remove(
            async_dispatcher_connect(
                self.hass, SIGNAL_CONNECTION, self._on_connection
            )
        )

    @callback
    def _on_connection(self, data: dict) -> None:
        """Handle UEFI client connection."""
        if self._offline_cancel:
            self._offline_cancel()
            self._offline_cancel = None

        self._attr_native_value = "online"
        self._attr_extra_state_attributes = {
            ATTR_LAST_BOOT_TIME: data["timestamp"],
            ATTR_LAST_CLIENT_IP: data["client_ip"],
        }
        self.async_write_ha_state()

        self._offline_cancel = async_call_later(
            self.hass, 60, self._set_offline
        )

    @callback
    def _set_offline(self, _now=None) -> None:
        """Set status back to offline after timeout."""
        self._offline_cancel = None
        self._attr_native_value = "offline"
        self.async_write_ha_state()
