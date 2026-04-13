#!/bin/zsh

set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
readonly PROJECT_DIR="${SCRIPT_DIR}"
readonly BASE_SDKCONFIG="${PROJECT_DIR}/sdkconfig"
readonly LOCAL_WIFI_SDKCONFIG="${PROJECT_DIR}/sdkconfig.local.wifi"
readonly LOCAL_WIFI_TEMPLATE="${PROJECT_DIR}/sdkconfig.local.wifi.example"

usage() {
  cat <<'EOF'
Usage: ./build_pipeline.sh [wifi|ethernet] [idf.py args...]

If no pipeline is provided, the script will ask.
If no idf.py args are provided, the default target is: build
EOF
}

choose_pipeline() {
  local answer
  local lowered_answer

  printf 'Choose build pipeline:\n' >&2
  printf '  1) wifi\n' >&2
  printf '  2) ethernet\n' >&2
  printf 'Selection [1/2]: ' >&2
  read -r answer
  lowered_answer="$(printf '%s' "$answer" | tr '[:upper:]' '[:lower:]')"

  case "$lowered_answer" in
    1|wifi|w)
      printf 'wifi\n'
      ;;
    2|ethernet|eth|e)
      printf 'ethernet\n'
      ;;
    *)
      printf 'Invalid selection: %s\n' "$answer" >&2
      return 1
      ;;
  esac
}

merge_sdkconfig() {
  local base_file="$1"
  local overlay_file="$2"
  local output_file="$3"

  mkdir -p "$(dirname "$output_file")"
  awk '
    function config_key(line, key) {
      if (line ~ /^CONFIG_[A-Za-z0-9_]+=/) {
        split(line, parts, "=")
        return parts[1]
      }
      if (line ~ /^# CONFIG_[A-Za-z0-9_]+ is not set$/) {
        key = line
        sub(/^# /, "", key)
        sub(/ is not set$/, "", key)
        return key
      }
      return ""
    }
    FNR == NR {
      overlay[++overlay_count] = $0
      key = config_key($0)
      if (key != "") {
        overrides[key] = 1
      }
      next
    }
    {
      key = config_key($0)
      if (key == "" || !(key in overrides)) {
        print
      }
    }
    END {
      for (i = 1; i <= overlay_count; ++i) {
        print overlay[i]
      }
    }
  ' "$overlay_file" "$base_file" > "$output_file"
}

config_enabled() {
  local config_file="$1"
  local key="$2"

  grep -q "^${key}=y$" "$config_file"
}

config_value() {
  local config_file="$1"
  local key="$2"
  local line

  line="$(grep -E "^${key}=" "$config_file" | tail -n 1 || true)"
  line="${line#*=}"
  line="${line#\"}"
  line="${line%\"}"
  printf '%s\n' "$line"
}

prepare_wifi_config() {
  local generated_file="$1"
  local tmp_file
  local ssid
  local password

  if config_enabled "$generated_file" "CONFIG_ENABLE_WIFI_PROVISIONING"; then
    return 0
  fi

  if [[ ! -f "$LOCAL_WIFI_SDKCONFIG" ]]; then
    printf 'Missing local Wi-Fi config: %s\n' "$LOCAL_WIFI_SDKCONFIG" >&2
    printf 'Copy %s to %s and set CONFIG_WIFI_SSID / CONFIG_WIFI_PASSWORD before building the wifi pipeline.\n' \
      "$LOCAL_WIFI_TEMPLATE" "$LOCAL_WIFI_SDKCONFIG" >&2
    exit 1
  fi

  tmp_file="${generated_file}.tmp"
  merge_sdkconfig "$generated_file" "$LOCAL_WIFI_SDKCONFIG" "$tmp_file"
  mv "$tmp_file" "$generated_file"

  ssid="$(config_value "$generated_file" "CONFIG_WIFI_SSID")"
  password="$(config_value "$generated_file" "CONFIG_WIFI_PASSWORD")"

  if [[ -z "$ssid" || -z "$password" || "$ssid" == "YOUR_WIFI_SSID" || "$password" == "YOUR_WIFI_PASSWORD" ]]; then
    printf 'Local Wi-Fi config %s must define real CONFIG_WIFI_SSID and CONFIG_WIFI_PASSWORD values.\n' \
      "$LOCAL_WIFI_SDKCONFIG" >&2
    exit 1
  fi
}

ensure_idf_env() {
  local default_idf_path="${HOME}/esp/esp-idf"
  local default_python_env="${HOME}/.espressif/python_env/idf5.1_py3.14_env"
  local had_nounset=0

  export IDF_PATH="${IDF_PATH:-$default_idf_path}"
  export IDF_PYTHON_ENV_PATH="${IDF_PYTHON_ENV_PATH:-$default_python_env}"

  if [[ ! -f "${IDF_PATH}/export.sh" ]]; then
    printf 'ESP-IDF export script not found: %s\n' "${IDF_PATH}/export.sh" >&2
    exit 1
  fi

  case "$-" in
    *u*)
      had_nounset=1
      set +u
      ;;
  esac

  # shellcheck disable=SC1090
  . "${IDF_PATH}/export.sh" >/dev/null

  if [[ "$had_nounset" -eq 1 ]]; then
    set -u
  fi
}

pipeline="${1:-}"
case "$pipeline" in
  wifi|ethernet)
    shift
    ;;
  "" )
    pipeline="$(choose_pipeline)"
    ;;
  -h|--help)
    usage
    exit 0
    ;;
  *)
    printf 'Unknown pipeline: %s\n\n' "$pipeline" >&2
    usage >&2
    exit 1
    ;;
esac

if [[ ! -f "$BASE_SDKCONFIG" ]]; then
  printf 'Base sdkconfig not found: %s\n' "$BASE_SDKCONFIG" >&2
  exit 1
fi

overlay_file="${PROJECT_DIR}/sdkconfig.overlay.${pipeline}"
build_dir="${PROJECT_DIR}/build-${pipeline}"
generated_sdkconfig="${build_dir}/sdkconfig.${pipeline}"

if [[ ! -f "$overlay_file" ]]; then
  printf 'Overlay file not found: %s\n' "$overlay_file" >&2
  exit 1
fi

if [[ $# -eq 0 ]]; then
  set -- build
fi

merge_sdkconfig "$BASE_SDKCONFIG" "$overlay_file" "$generated_sdkconfig"
if [[ "$pipeline" == "wifi" ]]; then
  prepare_wifi_config "$generated_sdkconfig"
fi
ensure_idf_env

printf 'Pipeline: %s\n' "$pipeline"
printf 'Build dir: %s\n' "$build_dir"
printf 'Config: %s\n' "$generated_sdkconfig"
if [[ "$pipeline" == "wifi" && -f "$LOCAL_WIFI_SDKCONFIG" ]]; then
  printf 'Local Wi-Fi config: %s\n' "$LOCAL_WIFI_SDKCONFIG"
fi
printf 'idf.py args: %s\n' "$*"

cd "$PROJECT_DIR"
idf.py -B "$build_dir" -DSDKCONFIG="$generated_sdkconfig" "$@"
