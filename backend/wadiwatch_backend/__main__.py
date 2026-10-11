"""Run the backend: python -m wadiwatch_backend

Settings come from environment variables, all optional:

  WADIWATCH_HOST           address to listen on (default 127.0.0.1)
  WADIWATCH_PORT           port to listen on (default 8000)
  WADIWATCH_DB             SQLite file (default backend/data/wadiwatch.db)
  WADIWATCH_SITE           site file (default backend/site/prototype.json)
  WADIWATCH_MQTT_HOST      MQTT broker to read the gateway uplink from and to
                           send the gateway its commands through; without it
                           the backend hears no gateway and no all clear can
                           be issued
  WADIWATCH_MQTT_PORT      broker port (default 1883)
  WADIWATCH_MQTT_USERNAME  broker sign-in, if the broker asks for one
  WADIWATCH_MQTT_PASSWORD
  WADIWATCH_COMMAND_KEY    the command key shared with the gateway, as 64
                           hexadecimal digits: every all clear command carries
                           an authentication code made with it, and the gateway
                           acts on no other. Without it the published test key
                           is used, which only a bench gateway built from the
                           repository as it is accepts. A real key is a secret:
                           set it here and in the gateway's local key file,
                           and never commit it
  WADIWATCH_HTTP_INGEST    set to 1 to serve POST /api/ingest, which takes
                           gateway messages over HTTP from anyone who can
                           reach it: for replaying a scenario on the local
                           machine only. Off unless set, and refused when
                           WADIWATCH_HOST is not the local machine
  WADIWATCH_GATEWAY        set to "simulated" to try the all clear without a
                           gateway: a stand-in answers the command the way the
                           gateway reports an all clear, and nothing else
                           happens (no radio, no siren, no SMS). Ignored when
                           WADIWATCH_MQTT_HOST is set
  WADIWATCH_SECURE_COOKIES set to 1 when the dashboard is served over HTTPS:
                           the officers' session cookie is then sent over
                           HTTPS only
"""

import logging
import os
import time

import uvicorn

from .app import HTTP_INGEST_SETTING, HttpIngestRefused, create_app, http_ingest_setting
from .backend import Backend
from .downlink import TEST_COMMAND_KEY, command_key_from
from .mqtt import DEFAULT_PORT, MqttUplink
from .simulated_gateway import SimulatedGateway
from .site import BACKEND_DIR, DEFAULT_SITE_FILE, load_site

DEFAULT_DB = BACKEND_DIR / "data" / "wadiwatch.db"
SIMULATED = "simulated"

log = logging.getLogger("wadiwatch")


def main():
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(message)s")
    env = os.environ
    host = env.get("WADIWATCH_HOST", "127.0.0.1")
    try:
        http_ingest = http_ingest_setting(env.get(HTTP_INGEST_SETTING), host)
    except HttpIngestRefused as refused:
        raise SystemExit(str(refused)) from None
    if http_ingest:
        log.warning(
            "POST /api/ingest is served without authentication: for a local replay only"
        )
    site = load_site(env.get("WADIWATCH_SITE", DEFAULT_SITE_FILE))
    command_key = command_key_from(env.get("WADIWATCH_COMMAND_KEY"))
    if command_key == TEST_COMMAND_KEY:
        log.warning(
            "all clear commands are signed with the published test key: "
            "set WADIWATCH_COMMAND_KEY for a real gateway"
        )
    backend = Backend(env.get("WADIWATCH_DB", DEFAULT_DB), site, command_key=command_key)
    uplink = None
    if env.get("WADIWATCH_MQTT_HOST"):
        uplink = MqttUplink(
            backend.ingest,
            host=env["WADIWATCH_MQTT_HOST"],
            port=int(env.get("WADIWATCH_MQTT_PORT", DEFAULT_PORT)),
            username=env.get("WADIWATCH_MQTT_USERNAME"),
            password=env.get("WADIWATCH_MQTT_PASSWORD"),
        )
        # Commands go down the connection the messages come up.
        backend.downlink = uplink.send_command
    elif env.get("WADIWATCH_GATEWAY") == SIMULATED:
        backend.downlink = SimulatedGateway(backend, time.time, command_key)
        log.warning("the gateway is simulated: an all clear stops no siren and sends no SMS")
    uvicorn.run(
        create_app(
            backend,
            uplink,
            secure_cookies=env.get("WADIWATCH_SECURE_COOKIES") == "1",
            http_ingest=http_ingest,
        ),
        host=host,
        port=int(env.get("WADIWATCH_PORT", "8000")),
        # The dashboard polls every two seconds; logging each request would bury the warnings.
        access_log=False,
    )


if __name__ == "__main__":
    main()
