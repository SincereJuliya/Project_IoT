import subprocess
import os
import sys
import matplotlib.pyplot as plt

# Config
LOG_FILE = '/home/sincerejuliya/loglistener.log' # log from cooja
IS_TESTBED = False                               # cooja
TIME_MARKERS = [3, 6, 9, 12, 15, 18]             # to choose minutes

PARSER_SCRIPT = 'parser.py'
ANALYSIS_SCRIPT = 'analysis.py'

def filter_log_by_time(input_log, output_log, max_seconds, is_testbed):
    import re
    from datetime import datetime, timedelta

    def parse_cooja_time(timestr):
        try:
            return datetime.strptime(timestr, "%H:%M:%S.%f")
        except ValueError:
            try:
                return datetime.strptime(timestr, "%M:%S.%f")
            except ValueError:
                print(f"Warning: Time data '{timestr}' does not match expected formats. Skipping line.")
                return None

    def parse_testbed_time(timestr):
        try:
            return datetime.strptime(timestr, "%H:%M:%S,%f")
        except ValueError:
            print(f"Warning: Time data '{timestr}' does not match testbed format. Skipping line.")
            return None

    # Choose regex and parser based on log type
    if is_testbed:
        time_pattern = re.compile(r"\[[0-9\-]+ (?P<time>[0-9:,]+)\]")
        time_parser = parse_testbed_time
    else:
        time_pattern = re.compile(r"^(?P<time>[0-9:.]+)")
        time_parser = parse_cooja_time

    start_time = None

    with open(input_log, 'r') as fin, open(output_log, 'w') as fout:
        for line in fin:
            m = time_pattern.search(line)
            if not m:
                continue
            timestr = m.group('time')
            dt = time_parser(timestr)
            if dt is None:
                continue
            if start_time is None:
                start_time = dt
            elapsed = (dt - start_time).total_seconds()
            if 0 <= elapsed <= max_seconds:
                fout.write(line)

def run_script(script, arg):
    print(f"Running {script} on {arg} ...")
    proc = subprocess.run([sys.executable, script, arg], capture_output=True, text=True)
    if proc.returncode != 0:
        # Instead of exiting, just print error and continue
        print(f"Warning: Error running {script}:")
        print(proc.stderr)
        # Return whatever output there is anyway
        return proc.stdout
    return proc.stdout

def parse_pdr_from_analysis_output(output):
    # Example parsing from overall PDR line like:
    # Overall PDR: 99.13% (3 LOST / 343 SENT)
    for line in output.splitlines():
        if "Overall PDR:" in line:
            import re
            m = re.search(r"Overall PDR:\s+([\d\.]+)% \((\d+) LOST / (\d+) SENT\)", line)
            if m:
                pdr = float(m.group(1))
                lost = int(m.group(2))
                sent = int(m.group(3))
                return pdr, lost, sent
    return None, None, None

def main():
    results = []

    for minutes in TIME_MARKERS:
        print(f"\n=== Processing up to {minutes} minutes ===")
        filtered_log = f"log_{minutes}min.log"
        filter_log_by_time(LOG_FILE, filtered_log, minutes * 60, IS_TESTBED)

        run_script(PARSER_SCRIPT, filtered_log)
        analysis_output = run_script(ANALYSIS_SCRIPT, '.')

        pdr, lost, sent = parse_pdr_from_analysis_output(analysis_output)
        if pdr is not None:
            results.append((minutes, pdr, lost, sent))
            print(f"Minute {minutes}: PDR={pdr}%, Lost={lost}, Sent={sent}")
        else:
            print(f"Could not parse PDR for minute {minutes}")

    # Plot results
    if results:
        mins, pdrs, losts, sents = zip(*results)
        plt.figure(figsize=(8,5))
        plt.plot(mins, pdrs, marker='o', label='Overall PDR (%)')
        # Annotate lost and sent near each point
        for x, pdr, lost, sent in zip(mins, pdrs, losts, sents):
            plt.annotate(
                f'Lost={lost}\nSent={sent}',
                (x, pdr),
                textcoords="offset points",
                xytext=(0,10),
                ha='center',
                fontsize=8,
                color='red'
            )
        plt.title('PDR over time intervals')
        plt.xlabel('Time (minutes)')
        plt.ylabel('Packet Delivery Ratio (%)')
        plt.grid(True)
        plt.legend()
        plt.tight_layout()
        plt.show()
    else:
        print("No results to plot.")

if __name__ == '__main__':
    main()
