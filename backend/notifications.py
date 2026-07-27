# NOTIFICATIONS MODULE
# Twilio SMS for backend-confirmed CUSUM alerts.
# Falls back to log-only if credentials aren't configured.

import logging

import config

logger = logging.getLogger(__name__)


def send_sms(message: str) -> bool:
    """
    Send an alert SMS via Twilio.
    Returns True if sent successfully, False otherwise.
    If SMS_ENABLED is False (no credentials configured), logs the message
    and returns False rather than raising.
    """
    if not config.SMS_ENABLED:
        logger.info("SMS disabled (no Twilio credentials configured). "
                     "Message would have been: %s", message)
        return False

    try:
        from twilio.rest import Client
        client = Client(config.TWILIO_ACCOUNT_SID, config.TWILIO_AUTH_TOKEN)

        msg = client.messages.create(
            body=message[:1600],     # Twilio supports up to 1600 chars
            from_=config.TWILIO_FROM_NUMBER,
            to=config.ALERT_PHONE_NUMBER,
        )

        logger.info("SMS sent via Twilio. SID: %s", msg.sid)
        return True

    except ImportError:
        logger.error("Twilio library not installed. Run: pip install twilio")
        return False

    except Exception:
        logger.exception("Twilio error while sending SMS")
        return False
