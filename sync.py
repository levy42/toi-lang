import subprocess
subprocess.run(
    ["rsync", "-az", "--delete", ".", "pi@pi3:/home/pi/toi"], check=True
)