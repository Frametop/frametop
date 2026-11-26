from dataclasses import dataclass


@dataclass(frozen=True)
class Profile:
    firmware_sha256: str = "4f9fd567eb582c24f1f960a6525fafb0bd75733223ffa7ce8d6541cfd2a62b1c"
    firmware_path: str = "/opt/steamvr/drivers/cv/bin/linuxarm64/cdsp_sm8650.mbn"
    qcom_sha256: str = "1b0d96ff0363e7db34a8890588e1279e3c67fb570ba6b7f0fc75c7b808cded6b"
    qcom_path: str = "/usr/lib/firmware/qcom/sm8650/cdsp.mbn"
    size: int = 0x2000000
    magic: int = 0x1614DF651D03F542
    abi: int = 9
    control: int = 0xA91000
    callback: int = 0xB12018
    code: int = 0xA92000
    code_size: int = 0x1000
    header: int = 0xA93000
    status: int = 0xA93400
    backup: int = 0xA94000
    arena: int = 0xA97000
    arena_size: int = 0x20000
    dsp_base: int = 0xF0000000
    dispatch_offset: int = 0x5A0
    dispatch_size: int = 96
    firmware_load_offset: int = 0x54
    dispatch_sha256: str = "ce08493d4ebc48bf77260abc8db8aa3a5fb7220f37a41d522f775ac8e7f7c3ad"


PROFILE = Profile()
JOB_MAGIC = 0x55504E46  # FNPU in little endian
JOB_ABI = 1
HMX = 1
DIRECT = 2
