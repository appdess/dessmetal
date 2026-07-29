import argparse
import importlib.util
import tempfile
import unittest
from pathlib import Path


ENTRYPOINT_PATH = Path(__file__).resolve().parents[1] / "cloud_train_entrypoint.py"
SPEC = importlib.util.spec_from_file_location("cloud_train_entrypoint", ENTRYPOINT_PATH)
ENTRYPOINT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ENTRYPOINT)


class SafeGCSDownloadPathTests(unittest.TestCase):
    def setUp(self):
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary_directory.cleanup)
        self.root = Path(self.temporary_directory.name) / "downloads"

    def test_maps_a_nested_object_below_the_exact_prefix(self):
        result = ENTRYPOINT.safe_gcs_download_path(
            "configs/default_config_files/learning/default.json",
            "configs",
            self.root,
        )

        self.assertEqual(
            result,
            self.root.resolve() / "default_config_files" / "learning" / "default.json",
        )
        self.assertFalse(result.parent.exists())

    def test_skips_prefix_and_directory_marker_objects(self):
        self.assertIsNone(
            ENTRYPOINT.safe_gcs_download_path("configs", "configs", self.root)
        )
        self.assertIsNone(
            ENTRYPOINT.safe_gcs_download_path("configs/nested/", "configs", self.root)
        )

    def test_rejects_prefix_confusion_and_path_traversal(self):
        unsafe_names = (
            "configs-archive/default.json",
            "configs/../../trainer_source/custom_train_full.py",
            "configs/./default.json",
            "configs//tmp/payload",
            "configs/..\\..\\payload",
            "configs/control\nname",
            "configs/../../trainer_source/",
        )

        for blob_name in unsafe_names:
            with self.subTest(blob_name=blob_name):
                with self.assertRaises(ValueError):
                    ENTRYPOINT.safe_gcs_download_path(blob_name, "configs", self.root)

        with self.assertRaises(ValueError):
            ENTRYPOINT.safe_gcs_download_path(
                "training-data/DessBlock-green/../../../trainer_source/custom_train_full.py",
                "training-data/DessBlock-green",
                self.root,
            )

    def test_rejects_a_destination_symlink(self):
        outside = Path(self.temporary_directory.name) / "outside"
        outside.mkdir()
        self.root.mkdir()
        (self.root / "escape").symlink_to(outside, target_is_directory=True)

        with self.assertRaises(ValueError):
            ENTRYPOINT.safe_gcs_download_path(
                "configs/escape/new-directory/payload.json",
                "configs",
                self.root,
            )
        self.assertFalse((outside / "new-directory").exists())

    def test_rejects_a_symlinked_destination_root(self):
        outside = Path(self.temporary_directory.name) / "outside-root"
        outside.mkdir()
        self.root.symlink_to(outside, target_is_directory=True)

        with self.assertRaises(ValueError):
            ENTRYPOINT.safe_gcs_download_path(
                "configs/payload.json",
                "configs",
                self.root,
            )

    def test_rejects_an_unsafe_source_prefix(self):
        for prefix in ("../configs", "/configs", "configs\\nested", "configs\narchive"):
            with self.subTest(prefix=prefix):
                with self.assertRaises(ValueError):
                    ENTRYPOINT.safe_gcs_download_path(
                        "configs/payload.json",
                        prefix,
                        self.root,
                    )


class FakeBlob:
    def __init__(self, name):
        self.name = name
        self.downloads = []

    def download_to_filename(self, filename):
        self.downloads.append(filename)
        Path(filename).write_text(self.name, encoding="utf-8")


class FakeBucket:
    def __init__(self, blobs):
        self.blobs = blobs
        self.prefixes = []

    def list_blobs(self, prefix):
        self.prefixes.append(prefix)
        return iter(self.blobs)


class FakeStorageClient:
    def __init__(self, blobs):
        self.fake_bucket = FakeBucket(blobs)

    def bucket(self, _bucket_name):
        return self.fake_bucket


class DownloadPreflightTests(unittest.TestCase):
    def setUp(self):
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary_directory.cleanup)
        self.root = Path(self.temporary_directory.name) / "downloads"

    def test_queries_the_exact_prefix_and_downloads_valid_objects(self):
        blob = FakeBlob("configs/nested/default.json")
        client = FakeStorageClient([blob])

        ENTRYPOINT.download_from_gcs("bucket", "configs", self.root, client)

        self.assertEqual(client.fake_bucket.prefixes, ["configs/"])
        self.assertEqual(len(blob.downloads), 1)
        self.assertEqual(
            Path(blob.downloads[0]).read_text(encoding="utf-8"),
            blob.name,
        )

    def test_malicious_late_object_causes_zero_downloads(self):
        valid = FakeBlob("configs/default.json")
        malicious = FakeBlob("configs/../../trainer_source/custom_train_full.py")
        client = FakeStorageClient([valid, malicious])

        with self.assertRaises(ValueError):
            ENTRYPOINT.download_from_gcs("bucket", "configs", self.root, client)

        self.assertEqual(valid.downloads, [])
        self.assertEqual(malicious.downloads, [])

    def test_file_directory_collisions_fail_before_download_in_either_order(self):
        for names in (
            ("configs/a", "configs/a/b"),
            ("configs/a/b", "configs/a"),
        ):
            with self.subTest(names=names):
                blobs = [FakeBlob(name) for name in names]
                client = FakeStorageClient(blobs)
                with self.assertRaises(ValueError):
                    ENTRYPOINT.download_from_gcs("bucket", "configs", self.root, client)
                self.assertTrue(all(blob.downloads == [] for blob in blobs))

    def test_duplicate_targets_fail_before_download(self):
        blobs = [FakeBlob("configs/default.json"), FakeBlob("configs/default.json")]
        client = FakeStorageClient(blobs)

        with self.assertRaises(ValueError):
            ENTRYPOINT.download_from_gcs("bucket", "configs", self.root, client)

        self.assertTrue(all(blob.downloads == [] for blob in blobs))


class ModelNameTests(unittest.TestCase):
    def test_accepts_current_model_names(self):
        for model_name in (
            "DessBlock-green",
            "DessBlock-red",
            "SickDess",
            "DessTortion-blue",
            "DessTortion-red",
            "TS9",
            "aesahaettr",
        ):
            with self.subTest(model_name=model_name):
                self.assertEqual(
                    ENTRYPOINT.validated_model_name(model_name),
                    model_name,
                )

    def test_rejects_model_names_that_can_change_paths(self):
        for model_name in ("../SickDess", "Dess/Sick", ".", "name with spaces"):
            with self.subTest(model_name=model_name):
                with self.assertRaises(argparse.ArgumentTypeError):
                    ENTRYPOINT.validated_model_name(model_name)


if __name__ == "__main__":
    unittest.main()
