"""Config flow for HA Boot Manager."""

from __future__ import annotations

import voluptuous as vol

from homeassistant import config_entries
from homeassistant.core import callback

from .const import (
    DOMAIN,
    CONF_TCP_PORT,
    CONF_DEFAULT_OS,
    DEFAULT_TCP_PORT,
    DEFAULT_DEFAULT_OS,
)


class HaBootManagerConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    """Handle a config flow for HA Boot Manager."""

    VERSION = 1

    async def async_step_user(self, user_input=None):
        """Handle the initial step."""
        # Only allow one instance
        if self._async_current_entries():
            return self.async_abort(reason="already_configured")

        errors = {}

        if user_input is not None:
            port = user_input[CONF_TCP_PORT]
            if port < 1024 or port > 65535:
                errors[CONF_TCP_PORT] = "invalid_port"
            else:
                return self.async_create_entry(
                    title="HA Boot Manager",
                    data=user_input,
                )

        return self.async_show_form(
            step_id="user",
            data_schema=vol.Schema(
                {
                    vol.Required(CONF_TCP_PORT, default=DEFAULT_TCP_PORT): int,
                    vol.Required(CONF_DEFAULT_OS, default=DEFAULT_DEFAULT_OS): str,
                }
            ),
            errors=errors,
        )

    @staticmethod
    @callback
    def async_get_options_flow(config_entry):
        """Get the options flow."""
        return HaBootManagerOptionsFlow(config_entry)


class HaBootManagerOptionsFlow(config_entries.OptionsFlow):
    """Handle options flow for HA Boot Manager."""

    def __init__(self, config_entry):
        """Initialize options flow."""
        self.config_entry = config_entry

    async def async_step_init(self, user_input=None):
        """Manage the options."""
        errors = {}

        if user_input is not None:
            port = user_input[CONF_TCP_PORT]
            if port < 1024 or port > 65535:
                errors[CONF_TCP_PORT] = "invalid_port"
            else:
                return self.async_create_entry(title="", data=user_input)

        current = self.config_entry.data

        return self.async_show_form(
            step_id="init",
            data_schema=vol.Schema(
                {
                    vol.Required(
                        CONF_TCP_PORT,
                        default=current.get(CONF_TCP_PORT, DEFAULT_TCP_PORT),
                    ): int,
                    vol.Required(
                        CONF_DEFAULT_OS,
                        default=current.get(CONF_DEFAULT_OS, DEFAULT_DEFAULT_OS),
                    ): str,
                }
            ),
            errors=errors,
        )
