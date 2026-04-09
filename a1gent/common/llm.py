from __future__ import annotations

import json
import os
from typing import Any, Dict, Optional

from openai import OpenAI

from .log import setup_logger

log = setup_logger("llm")

_client: Optional[OpenAI] = None
_extra_headers: Optional[Dict[str, str]] = None


def get_openrouter_client() -> Optional[OpenAI]:
    """Initialise (once) and return the shared OpenRouter client."""
    global _client, _extra_headers
    if _client is not None:
        return _client

    api_key = os.getenv("OPENROUTER_API_KEY")
    if not api_key:
        log.warning("OpenRouter client disabled: OPENROUTER_API_KEY not set")
        return None

    base_url = os.getenv("OPENROUTER_BASE_URL", "https://openrouter.ai/api/v1")
    try:
        _client = OpenAI(base_url=base_url, api_key=api_key)
    except Exception as e:
        log.error("Failed to initialise OpenRouter client: %s", e)
        _client = None
        return None

    headers: Dict[str, str] = {}
    referer = os.getenv("OPENROUTER_SITE_URL")
    title = os.getenv("OPENROUTER_SITE_NAME")
    if referer:
        headers["HTTP-Referer"] = referer
    if title:
        headers["X-Title"] = title
    _extra_headers = headers or None
    return _client


def structured_completion(
    *,
    model: str,
    system_prompt: str,
    user_content: str,
    schema_name: str,
    schema: Dict[str, Any],
    temperature: float = 0.0,
) -> Optional[Dict[str, Any]]:
    """
    Call OpenRouter with a JSON-schema constrained request.

    Returns the parsed JSON object if successful; otherwise None.
    """
    client = get_openrouter_client()
    if not client:
        return None

    try:
        response = client.chat.completions.create(
            model=model,
            messages=[
                {"role": "system", "content": system_prompt},
                {"role": "user", "content": user_content},
            ],
            response_format={
                "type": "json_schema",
                "json_schema": {
                    "name": schema_name,
                    "strict": True,
                    "schema": schema,
                },
            },
            # Budget controls
            max_tokens=int(os.getenv("LLM_ANSWER_MAX", "2048")),  # tokens for final JSON
            extra_body={
                "reasoning": {
                    # works for Gemini thinking + Anthropic; ignored by non-thinking models
                    "max_tokens": int(os.getenv("LLM_REASONING_MAX", "384")),  # hidden thinking budget
                    "exclude": True  # think internally; do NOT return reasoning text
                }
            },
            temperature=temperature,
            extra_headers=_extra_headers or {},
        )
    except Exception as e:
        log.warning("OpenRouter request failed: %s", e)
        return None

    choice = response.choices[0]
    content = choice.message.content

    if isinstance(content, str):
        raw = content
    else:
        parts = []
        for item in content or []:
            if isinstance(item, dict):
                text = item.get("text")
                if text:
                    parts.append(text)
        raw = "".join(parts)

    if not raw:
        log.warning("OpenRouter returned empty content")
        return None

    try:
        return json.loads(raw)
    except Exception as e:
        log.warning("OpenRouter response parse error: %s | payload=%s", e, raw)
        return None
