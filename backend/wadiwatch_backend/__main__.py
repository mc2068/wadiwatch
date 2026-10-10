"""Run the backend: python -m wadiwatch_backend

Settings come from environment variables, all optional:

  WADIWATCH_HOST           address to listen on (default 127.0.0.1)
  WADIWATCH_PORT           port to listen on (default 8000)
  WADIWATCH_DB             SQLite file (default backend/data/wadiwatch.db)
  WADIWATCH_SITE           site file (default backend/site/prototype.json)
  WADIWATCH_MQTT_HOST      MQTT broker to read the gateway uplink from and to
                           send the gateway its commands through; without it
                           only the HTTP ingest endpoint is fed and no all
                           clear can be issued
  WADIWATCH_MQTT_PORT      broker port (default 1883)
  WADIWATCH_MQTT_USERNAME  broker sign-in, if the broker asks for one
  WADIWATCH_MQTT_PASSWORD
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

from .app import create_app
from .backend import Backend
from .mqtt import DEFAULT_PORT, MqttUplink
from .simulated_gateway import SimulatedGateway
from .site import BACKEND_DIR, DEFAULT_SITE_FILE, load_site

DEFAULT_DB = BACKEND_DIR / "data" / "wadiwatch.db"
SIMULATED = "simulated"

log = logging.getLogger("wadiwatch")


def main():
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(message)s")
    env = os.environ
    site = load_site(env.get("WADIWATCH_SITE", DEFAULT_SITE_FILE))
    backend = Backend(env.get("WADIWATCH_DB", DEFAULT_DB), site)
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
        backend.downlink = SimulatedGateway(backend, time.time)
        log.warning("the gateway is simulated: an all clear stops no siren and sends no SMS")
    uvicorn.run(
        create_app(backend, uplink, secure_cookies=env.get("WADIWATCH_SECURE_COOKIES") == "1"),
        host=env.get("WADIWATCH_HOST", "127.0.0.1"),
        port=int(env.get("WADIWATCH_PORT", "8000")),
        # The dashboard polls every two seconds; logging each request would bury the warnings.
        access_log=False,
    )


if __name__ == "__main__":
    main()
