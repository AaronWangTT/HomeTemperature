import unittest
from pathlib import Path


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

    def test_public_release_metadata_exists(self) -> None:
        required_paths = (
            "LICENSE",
            "README.md",
            "SECURITY.md",
            "THIRD_PARTY_NOTICES.md",
            ".github/workflows/ci.yml",
            ".github/workflows/secret-scan.yml",
        )

        for relative_path in required_paths:
            with self.subTest(path=relative_path):
                self.assertTrue((REPOSITORY_ROOT / relative_path).is_file())


if __name__ == "__main__":
    unittest.main()