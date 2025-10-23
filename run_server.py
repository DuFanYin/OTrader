#!/usr/bin/env python3
"""
Startup script for FACTT backend server with frontend
"""
import subprocess
import sys
from pathlib import Path


def main():
    """Start the FACTT backend server"""
    backend_dir = Path(__file__).parent / "backend"

    # Check if backend directory exists
    if not backend_dir.exists():
        print("Error: Backend directory not found")
        sys.exit(1)

    # Use conda environment python
    conda_python = "/opt/anaconda3/envs/Dev-env/bin/python"

    # Start the server
    try:
        subprocess.run(
            [
                conda_python,
                "-m",
                "uvicorn",
                "src.main:app",
                "--host",
                "127.0.0.1",
                "--port",
                "8080",
                "--reload",
            ],
            cwd=backend_dir,
            check=True,
        )
    except subprocess.CalledProcessError as e:
        print(f"Error starting server: {e}")
        sys.exit(1)
    except KeyboardInterrupt:
        print("\nServer stopped")
        sys.exit(0)


if __name__ == "__main__":
    main()
