# IoT Routing Protocol Project

This project was developed for the course **Low-Power Wireless Networking for the Internet of Things** (University of Trento, AY 2024–25).  
It focuses on the implementation and evaluation of a custom low-power routing protocol in **Contiki-NG** and the **Cooja** simulator.

---

## Project Overview

- **Protocol implementation:** Custom routing protocol developed in `rp.c` and `rp.h` (main contribution)
- **Provided by instructor:**  
  - Application logic in `app.c`  
  - Analysis tools: `analysis.py`, `parser.py`, `energest-stats.py`  
- Simulation configurations: `test.csc` and `test_nogui_dc.csc`
- Energest monitoring support in `tools/simple-energest.c` and `tools/simple-energest.h`

---

## Getting Started

### Prerequisites

- [Contiki-NG](https://github.com/contiki-ng/contiki-ng) installed and set up
- [Cooja simulator](https://github.com/contiki-ng/cooja)
- Python 3 with necessary libraries (e.g., pandas, matplotlib) for analysis scripts

### Build and Run

1. Build the project to run it in cooja (for testbed TARGET=zoul):
    ```bash
    make TARGET=sky
    ```

2. Open and run simulations in Cooja:
    - Load `test.csc` for GUI simulation
    - Use `test_nogui_dc.csc` for headless simulation

3. Collect logs from the simulation serial output.

4. Use Python scripts to analyze the logs and evaluate performance:
    ```bash
    python3 parser.py <logfile>
    python3 analysis.py <parsed_data>
    python3 pdr_time_analysis.py <parsed_data>
    python3 energest-stats.py <energystats_log>
    ```

---

## Documentation

A detailed article describing the project, design choices, and evaluation is included in the `docs/` folder:

- [Article PDF](docs/IoT_Sharipova_doc.pdf)

---

## Project Structure

| File/Folder                 | Description                               |
|----------------------------|-------------------------------------------|
| `rp.c`, `rp.h`             | Custom routing protocol                   |
| `app.c`                    | Application logic (provided by instructor) |
| `project-conf.h`           | Project configuration                     |
| `Makefile`                 | Build script                             |
| `test.csc`                 | Cooja GUI simulation file                 |
| `test_nogui_dc.csc`        | Cooja headless simulation                 |
| `analysis.py`              | Analysis tools (provided by instructor)  |
| `parser.py`                | Log parsing (provided by instructor)     |
| `pdr_time_analysis.py`     | Packet Delivery Ratio over time           |
| `energest-stats.py`        | Energy consumption analysis (provided by instructor) |
| `tools/simple-energest.c`  | Energest monitoring source                |
| `tools/simple-energest.h`  | Energest monitoring header                |
| `README.md`                | Project documentation                      |

---

## Notes

- The folders `testbed_files/` and `testbed_json/` are **not included** as they were provided by the course and are not original work.
- Modify simulation files to fit your experiment needs.

---

## License

This project is for academic purposes and educational use only.
