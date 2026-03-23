"""Select entity for HA Boot Manager - OS selection dropdown."""

from __future__ import annotations

from homeassistant.components.select import SelectEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.dispatcher import async_dispatcher_connect
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.restore_state import RestoreEntity

from .const import DOMAIN, CONF_DEFAULT_OS, DEFAULT_DEFAULT_OS, SIGNAL_OS_LIST_UPDATE


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities: AddEntitiesCallback,
) -> None:
    """Set up the Boot OS select entity."""
    default_os = entry.data.get(CONF_DEFAULT_OS, DEFAULT_DEFAULT_OS)
    entity = BootOsSelectEntity(entry, default_os)
    hass.data[DOMAIN]["select_entity"] = entity
    async_add_entities([entity])


class BootOsSelectEntity(SelectEntity, RestoreEntity):
    """Select entity for choosing which OS to boot."""

    _attr_has_entity_name = True
    _attr_name = "Boot OS"
    _attr_icon = "mdi:desktop-tower"

    def __init__(self, entry: ConfigEntry, default_os: str) -> None:
        """Initialize the select entity."""
        self._entry = entry
        self._default_os = default_os
        self._attr_unique_id = f"{DOMAIN}_boot_os"
        self._attr_options = [default_os]
        self._attr_current_option = default_os

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
        """Restore state and subscribe to updates."""
        last_state = await self.async_get_last_state()
        if last_state and last_state.state not in ("unknown", "unavailable"):
            self._attr_current_option = last_state.state
            if last_state.attributes.get("options"):
                self._attr_options = list(last_state.attributes["options"])

        self.async_on_remove(
            async_dispatcher_connect(
                self.hass, SIGNAL_OS_LIST_UPDATE, self._update_options
            )
        )

    @callback
    def _update_options(self, os_list: list[str]) -> None:
        """Update options when UEFI sends a new OS list."""
        self._attr_options = os_list
        if self._attr_current_option not in os_list:
            self._attr_current_option = os_list[0] if os_list else self._default_os
        self.async_write_ha_state()

    async def async_select_option(self, option: str) -> None:
        """Handle user selecting an OS."""
        self._attr_current_option = option
        self.async_write_ha_state()
