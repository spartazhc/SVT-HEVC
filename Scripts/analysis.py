#!/usr/bin/python3
import sys
import os
import subprocess

processes=["RES", "PA", "PD", "ME", "IRC", "SRC", "PM", "RC",
            "MDC", "ENC", "ENT", "PAK"]
processes_full=["RESOURCE", "PA", "PD", "ME", "IRC", "SRC", "PM", "RC",
            "MDC", "ENCDEC", "ENTROPY", "PAK"]
latency_label=["RESOURCE", "PA", "PD", "ME", "IRC", "SRC", "PM", "RC",
            "MDC", "ENCDEC", "ENTROPY", "PAK", "PAK_sche", "PM_sche", "PD_sche", "overhead"]
def pname_to_index(name):
    for i, p in enumerate(processes_full):
        if p == name:
            return i
    return -1
class TimeEntry(object):
    def __init__(self, record):
        rec = record.split(', ')
        self.proc = rec[0]
        self.in_type = int(rec[1])
        self.out_type = int(rec[2])
        self.pic_num = int(rec[3])
        self.seg_idx = int(rec[4])
        self.tile_idx = int(rec[5])
        self.stime = float(rec[6])
        self.etime = float(rec[7])
        self.duration = float(rec[8])

# record for every process in each frame
class Record(object):
    def __init__(self, name):
        self.name = name
        self.stime = 0
        self.stime_flag = 0
        self.etime = 0
        self.cputime = 0
    def update_record(self, entry):
        if (self.stime_flag == 0):
            self.stime = entry.stime
            self.stime_flag = 1
        if (entry.etime > self.etime):
            self.etime = entry.etime
        self.cputime += entry.duration
    def update_cputime_only(self, entry):
        self.cputime += entry.duration
    def get_cputime(self):
        return self.cputime
    def get_latency(self):
        return self.etime - self.stime

class FrameInfo(object):
    def __init__(self, POC):
        self.POC = POC
        self.start_time = 0
        self.end_time = 0
        self.cpu_time = 0
        self.records = []
        for p in processes:
             self.records.append(Record(p))


    def process_entry(self, entry):
        pindex = pname_to_index(entry.proc)
        # skip items which will cause double count
        if (entry.proc == "ENTROPY" and entry.out_type == 0):
            return
        elif (entry.proc == "ENCDEC" and entry.out_type == 0):
            return
        self.records[pindex].update_record(entry)
        return
    def get_flatency(self):
        return (self.records[-1].etime - self.records[0].stime)
    def get_pd_lat(self):
        return (self.records[2].stime - self.records[1].etime)
    def get_pm_lat(self):
        return (self.records[6].stime - self.records[5].etime)
    def get_pak_lat(self):
        return (self.records[11].stime - self.records[10].etime)
    def get_proc_gap(self, pi):
        return (self.records[pi].stime - self.records[pi-1].etime)

    def get_fcputime(self):
        cputime = 0
        for rec in self.records:
            cputime += rec.get_cputime()
        return cputime

def autopct_generator(limit):
    """Remove percent on small slices."""
    def inner_autopct(pct):
        return ('%.2f%%' % pct) if pct > limit else ''
    return inner_autopct

def main():
    if (len(sys.argv) != 3):
        print("usage: cmd + raw.csv + frame_count")
    raw_file = sys.argv[1]
    frames = int(sys.argv[2])

    dir_name = os.path.basename(raw_file).split('.')[0] + "-ana"
    if (os.path.isdir(dir_name)):
        subprocess.call(f"rm -rf {dir_name}", shell=True)
    os.mkdir(dir_name)
    os.mkdir(os.path.join(dir_name, "frames"))
    grep_frames = frames-1 if frames < 30 else 30

    for poc in range(0, grep_frames):
        subprocess.call(f"grep -P '[A-Z]+\, -?\d\, -?\d\, {poc}\,.*' {raw_file} > {dir_name}/frames/frame{poc:02d}.csv", shell=True)

    frame_list = []
    frame_cputime = 0.0
    frame_latency = []
    process_latency = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
    process_cputime = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
    for i in range(0, frames):
        frame_list.append(FrameInfo(i))
    with open(raw_file, "rt") as in_file:
        next(in_file)
        for line in in_file:
            entry = TimeEntry(line)
            frame_list[entry.pic_num].process_entry(entry)

    for i in range(0, frames):
        for pi in range(0, len(processes)):
            process_latency[pi] += frame_list[i].records[pi].get_latency()
            process_cputime[pi] += frame_list[i].records[pi].get_cputime()
        frame_cputime += frame_list[i].get_fcputime()
        frame_latency.append(frame_list[i].get_flatency())
        process_latency[-4] += frame_list[i].get_pak_lat()
        process_latency[-3] += frame_list[i].get_pm_lat()
        process_latency[-2] += frame_list[i].get_proc_gap(4)

    # do not use the last frame
    frames -= 1

    fd_cpu = open(os.path.join(dir_name, "cputime.csv"), "wt")
    fd_lat = open(os.path.join(dir_name, "latency.csv"), "wt")
    fd_cpu.write("POC,    CPU, RES,  PA,  PD,   ME, IRC, SRC,  PM,  RC,  MDC,  ENC,  ENT, PAK\n")
    for poc in range(0, frames):
        fd_cpu.write(f"{poc:3d}, {frame_list[poc].get_fcputime():6.1f},")
        for p in range(0, len(processes)):
            if (p == 3 or 8 <= p <= 10):
                fd_cpu.write(f"{frame_list[poc].records[p].get_cputime():5.1f},")
            else:
                fd_cpu.write(f"{frame_list[poc].records[p].get_cputime():4.1f},")
        fd_cpu.write("\n")

    fd_cpu.write(f"Avg, {frame_cputime/frames:6.1f},")
    for p in range(0, len(processes)):
        if (p == 3 or 8 <= p <= 10):
            fd_cpu.write(f"{process_cputime[p]/frames:5.1f},")
        else:
            fd_cpu.write(f"{process_cputime[p]/frames:4.1f},")
    fd_cpu.write("\n")

    fd_lat.write("POC,   CPU,  RES,   PA,   PD,   ME,  IRC,  SRC,   PM,   RC,  MDC,  ENC,  ENT,  PAK,pak_s, pm_s,irc_s\n")
    for poc in range(0, frames):
        fd_lat.write(f"{poc:3d}, {frame_list[poc].get_flatency():5.1f},")
        for p in range(0, len(processes)):
            fd_lat.write(f"{frame_list[poc].records[p].get_latency():5.2f},")
        fd_lat.write(f"{frame_list[poc].get_pak_lat():5.2f},{frame_list[poc].get_pm_lat():5.2f},{frame_list[poc].get_proc_gap(4):5.2f}\n")
    fd_lat.write(f"Avg, {sum(frame_latency)/frames:5.1f},")
    # negative because ENC and ENT have parallel part
    process_latency[-1] = sum(frame_latency) - sum(process_latency)
    for p in range(0, len(process_latency)):
        fd_lat.write(f"{process_latency[p]/frames:5.2f},")

    # check proc gap
    # print("POC,   PA,   PD,   ME,  IRC,  SRC,   PM,   RC,  MDC,  ENC,  ENT,  PAK")
    # for poc in range(0, frames):
    #     print(f"{poc:3d}", end=", ")
    #     for p in range(1, len(processes)):
    #         print(f"{frame_list[poc].get_proc_gap(p):5.2f}", end=",")
    #     print("")

if __name__ == '__main__':
    main()
