import subprocess
subprocess.run(
    ["rsync", "-az", "--delete", ".", "pi@pi3:/home/pi/mylang"], check=True
)