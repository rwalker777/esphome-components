"""Stage private device configs for a pinned UPS firmware hardware trial."""

import argparse
from copy import deepcopy
from pathlib import Path
import re
import shutil

from ruamel.yaml import YAML
from ruamel.yaml.comments import TaggedScalar
from ruamel.yaml.scalarstring import LiteralScalarString


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--revision", required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9a-f]{40}", args.revision):
        parser.error("revision must be a full commit SHA")
    source = args.source.resolve()
    if not (source / "configs/tripplite.yaml").is_file():
        parser.error("source must contain the Tripp Lite package")
    if args.output.resolve().is_relative_to(source):
        parser.error("private output must be outside the source checkout")
    yaml = YAML()
    config = yaml.load(args.device)
    if set(config.get("packages", {})) != {"tripplite"}:
        parser.error("device must use only the tripplite package")
    args.output.mkdir(mode=0o700, parents=True, exist_ok=False)
    output = args.output.resolve()
    shutil.copy2(args.device, output / "original-device.yaml")
    secrets = args.device.parent / "secrets.yaml"
    if secrets.is_file():
        shutil.copy2(secrets, output / "secrets.yaml")

    config.setdefault("substitutions", {})["ups_component_source"] = (
        f"github://kevin-david/esphome-components@{args.revision}"
    )
    config["packages"] = {"tripplite": {
        "url": "https://github.com/kevin-david/esphome-components",
        "ref": args.revision, "files": ["configs/tripplite.yaml"], "refresh": "0s",
    }}
    config["esphome"]["project"] = {
        "name": "kevin.ups_hid", "version": args.revision[:12],
    }
    yaml.dump(config, output / "pinned-device.yaml")

    for name in ("probe", "final"):
        staged = deepcopy(config)
        staged["substitutions"]["ups_component_source"] = str(source / "components")
        staged["packages"] = {"tripplite": TaggedScalar(
            str(source / "configs/tripplite.yaml"), tag="!include",
        )}
        staged["esphome"]["build_path"] = str(output / f"{name}-build")
        if name == "probe":
            staged["esphome"]["project"]["version"] += "-probe"
            staged.setdefault("logger", {})["level"] = "DEBUG"
            staged["api"].setdefault("actions", []).append({
                "action": "test_usb_disconnect",
                "then": [{"lambda": LiteralScalarString(
                    'auto ret = usb_host_lib_set_root_port_power(false);\n'
                    'ESP_LOGI("ups_hid.usb_test", "USB disconnect injection: %s", esp_err_to_name(ret));\n'
                )}],
            })
        yaml.dump(staged, output / f"{name}.yaml")
    print(f"Prepared {args.revision} in {output}; device credentials stayed in this directory")


if __name__ == "__main__":
    main()
