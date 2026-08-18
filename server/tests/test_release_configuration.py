import unittest
from pathlib import Path
import subprocess


SERVER_ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = SERVER_ROOT.parent


class ReleaseConfigurationTests(unittest.TestCase):
    def test_device_key_is_removed_from_access_logs(self) -> None:
        caddyfile = (SERVER_ROOT / "Caddyfile").read_text(encoding="utf-8")

        self.assertIn("format filter", caddyfile)
        self.assertIn("request>headers>X-Device-Key delete", caddyfile)
        self.assertIn("{$PUBLIC_HOST}", caddyfile)
        self.assertNotIn("cloudapp.azure.com", caddyfile)

    def test_runtime_data_is_excluded_from_source_control(self) -> None:
        gitignore = (REPOSITORY_ROOT / ".gitignore").read_text(encoding="utf-8")

        self.assertIn("server/backups/", gitignore)
        self.assertIn("server/**/*.db-*", gitignore)
        self.assertIn("server/.env.*", gitignore)
        self.assertIn(".pytest_cache/", gitignore)

    def test_runtime_data_is_excluded_from_container_context(self) -> None:
        dockerignore = (SERVER_ROOT / ".dockerignore").read_text(encoding="utf-8")

        self.assertIn("**/*.db-*", dockerignore)
        self.assertIn("backups/", dockerignore)
        self.assertIn(".env.*", dockerignore)
        self.assertIn("!.env.example", dockerignore)

    def test_deployment_rejects_public_placeholders(self) -> None:
        deploy_script = (SERVER_ROOT / "scripts" / "deploy.sh").read_text(encoding="utf-8")

        self.assertIn("telemetry\\.example\\.com", deploy_script)

    def test_deployment_validates_environment_and_waits_for_health(self) -> None:
        deploy_script = (SERVER_ROOT / "scripts" / "deploy.sh").read_text(encoding="utf-8")

        for variable in (
            "ACME_EMAIL",
            "PUBLIC_HOST",
            "DEVICE_API_KEY",
            "DASHBOARD_USERNAME",
            "DASHBOARD_PASSWORD",
        ):
            with self.subTest(variable=variable):
                self.assertIn(variable, deploy_script)

        self.assertIn('grep -c "^${variable}=" .env', deploy_script)
        self.assertIn('env_mode=$(stat -c \'%a\' .env)', deploy_script)
        self.assertIn(".env must have mode 600", deploy_script)
        self.assertIn("must contain exactly one", deploy_script)
        self.assertIn("contains an empty", deploy_script)
        self.assertIn("docker compose up --help", deploy_script)
        self.assertIn("docker compose pull caddy", deploy_script)
        self.assertIn("--wait", deploy_script)
        self.assertIn("--wait-timeout", deploy_script)

    def test_api_image_records_release_revision(self) -> None:
        dockerfile = (SERVER_ROOT / "Dockerfile").read_text(encoding="utf-8")
        compose = (SERVER_ROOT / "compose.yaml").read_text(encoding="utf-8")

        self.assertIn("ARG VCS_REF=unknown", dockerfile)
        self.assertIn('org.opencontainers.image.revision="${VCS_REF}"', dockerfile)
        self.assertIn("image: az3166-gateway-api:latest", compose)

    def test_shell_scripts_are_lf_and_executable_in_git(self) -> None:
        tracked_paths = subprocess.run(
            ["git", "ls-files", "--", "server"],
            cwd=REPOSITORY_ROOT,
            check=True,
            capture_output=True,
            text=True,
        ).stdout.splitlines()
        shell_scripts = tuple(
            REPOSITORY_ROOT / path for path in tracked_paths if path.endswith(".sh")
        )
        self.assertTrue(shell_scripts)

        for script in shell_scripts:
            relative_path = script.relative_to(REPOSITORY_ROOT).as_posix()
            content = script.read_bytes()
            index_entry = subprocess.run(
                ["git", "ls-files", "--stage", "--", relative_path],
                cwd=REPOSITORY_ROOT,
                check=True,
                capture_output=True,
                text=True,
            ).stdout.strip()

            with self.subTest(path=relative_path):
                self.assertTrue(content.startswith(b"#!/usr/bin/env bash\n"))
                self.assertNotIn(b"\r", content)
                self.assertTrue(index_entry)
                self.assertEqual("100755", index_entry.split(maxsplit=1)[0])

    def test_public_release_metadata_exists(self) -> None:
        required_paths = (
            "LICENSE",
            "README.md",
            "SECURITY.md",
            "THIRD_PARTY_NOTICES.md",
            ".github/workflows/ci.yml",
            ".github/workflows/secret-scan.yml",
            ".github/skills/production-deployment/SKILL.md",
            "server/tools/New-ServerReleaseArchive.ps1",
        )

        for relative_path in required_paths:
            with self.subTest(path=relative_path):
                self.assertTrue((REPOSITORY_ROOT / relative_path).is_file())


if __name__ == "__main__":
    unittest.main()