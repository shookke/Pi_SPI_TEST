import spidev
import struct
import os
import time
import RPi.GPIO as GPIO
from picamera2 import Picamera2, Preview
from libcamera import Transform
from datetime import datetime
import subprocess

# SPI setup
spi = spidev.SpiDev()
spi.open(0, 0)
spi.max_speed_hz = 1000000  # 1 MHz
spi.mode = 0b00  # SPI mode 0

# Command and ACK bytes (must match the ESP32 code)
WRITE_CMD = 0xA1          # Command to signal readiness to write image data
ACK_BYTE = 0xAC           # Initial acknowledgment byte from ESP32
READY_ACK_BYTE = 0xAD     # Ready acknowledgment byte from ESP32 after image size
FINAL_ACK_BYTE = 0xAF     # Final acknowledgment byte from ESP32 after image data
CHECKSUM_ACK_BYTE = 0xAE  # Checksum ready acknowledgment byte from ESP32
BUF_SIZE = 4096           # 4KB buffer size

camera = Picamera2()
still_config = camera.create_still_configuration(main={"size": (1080, 1920)}, transform=Transform(hflip=False, vflip=True))
camera.configure(still_config)
camera.set_controls({"AfMode": 2})

# Function to capture image
def capture_image(image_path):
    # Capture image with autofocus
    camera.start()
    time.sleep(2)  # Let the camera adjust to lighting and focus
    camera.capture_file(image_path)
    camera.stop()

    return True

def send_command(command):
    """
    Send a command byte to the ESP32 over SPI.
    """
    print(f"Sending command: 0x{command:X}")
    response = spi.xfer2([command])
    print(f"Received response: {response}")

def wait_for_ack(expected_ack):
    """
    Wait for an acknowledgment (ACK) from the ESP32.
    """
    print(f"Waiting for ACK 0x{expected_ack:X} from ESP32...")
    while True:
        # Send a dummy byte to clock the SPI bus and read the response
        response = spi.xfer2([0x00])
        if expected_ack in response:
            print(f"Received ACK 0x{expected_ack:X} from ESP32.")
            return True
        else:
            print(f"Received unexpected response: {response}")
        time.sleep(0.001)

def send_image_size(image_path):
    """
    Send the image size to the ESP32.
    """
    if not os.path.exists(image_path):
        print(f"Error: Image file {image_path} not found.")
        return False

    # Get image size
    image_size = os.path.getsize(image_path)
    print(f"Image size: {image_size} bytes")

    # Send image size (4 bytes, big-endian)
    size_bytes = struct.pack('>I', image_size)
    response = spi.xfer2(list(size_bytes))
    print(f"Sent image size: {image_size} bytes.")
    return True

def calculate_checksum(image_path):
    """
    Calculate the checksum of the image using a simple addition of all bytes.
    """
    checksum = 0
    with open(image_path, 'rb') as f:
        while chunk := f.read(BUF_SIZE):
            for byte in chunk:
                checksum += byte
    return checksum

def send_image(image_path):
    """
    Send the image data to the ESP32.
    """
    if not os.path.exists(image_path):
        print(f"Error: Image file {image_path} not found.")
        return False

    # Send image data in chunks
    with open(image_path, 'rb') as image_file:
        total_bytes_sent = 0
        while True:
            chunk = image_file.read(BUF_SIZE)
            if not chunk:
                break
            response = spi.xfer2(list(chunk))
            total_bytes_sent += len(chunk)
            #print(f"Sent {len(chunk)} bytes of image data, total sent: {total_bytes_sent} bytes.")
            time.sleep(0.001)  # Delay to allow ESP32 to process the data

    print("Image transfer complete.")
    return True

def send_checksum(checksum):
    """
    Send the calculated checksum to the ESP32.
    """
    checksum_bytes = struct.pack('>I', checksum)  # Pack checksum as 4 bytes (big-endian)
    print(f"Sending checksum: 0x{checksum:08X}")
    response = spi.xfer2(list(checksum_bytes))
    print(f"Sent checksum: 0x{checksum:08X}. Received response: {response}")
    return True

if __name__ == "__main__":
    image_path = 'captured_image.jpg'

    # Step 1: Capture the image
    if capture_image(image_path):
        # Step 2: Send a write command to the ESP32
        send_command(WRITE_CMD)

        # Step 3: Wait for the ESP32 to acknowledge the command (ACK_BYTE)
        if wait_for_ack(ACK_BYTE):
            # Step 4: Send the image size
            if send_image_size(image_path):
                # Step 5: Wait for the ESP32 to be ready (READY_ACK_BYTE)
                if wait_for_ack(READY_ACK_BYTE):
                    # Step 6: Send the image data
                    if send_image(image_path):
                        print("Completed Image Transfer.")
                    else:
                        print("Failed to send image data.")
                else:
                    print("Failed to receive READY ACK from ESP32.")
            else:
                print("Failed to send image size.")
        else:
            print("Failed to receive initial ACK from ESP32.")