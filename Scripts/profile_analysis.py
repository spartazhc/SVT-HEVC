#!/usr/bin/python3
import sys

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


class FrameInfo(object):
    def __init__(self, POC):
        self.POC = POC
        self.start_time = 0
        self.end_time = 0
        self.cpu_time = 0

        self.encdec_stime = 9999999
        self.encdec_etime = 0
        self.encdec_cputime = 0
        self.me_stime = 0
        self.me_etime = 0
        self.me_cputime = 0
        self.resource_latency = 0
        self.pa_latency = 0
        self.entropy_stime = 9999999
        self.entropy_etime = 0
        # self.entry = []
    def process_entry(self, entry):
        if (entry.proc == "RESOURCE"):
            self.start_time = entry.stime
            self.resource_latency = entry.etime - entry.stime
        elif (entry.proc == "PA"):
            self.pa_latency = entry.etime - entry.stime
        elif (entry.proc == "PAK"):
            self.end_time = entry.etime
        elif (entry.proc == "ME"):
            if (entry.seg_idx == 0):
                self.me_stime = entry.stime
            if (entry.etime > self.me_etime):
                self.me_etime = entry.etime
            self.me_cputime += entry.duration
        elif (entry.proc == "ENTROPY" and entry.out_type == 0):
            self.entropy_etime = entry.etime
            return
        elif (entry.proc == "ENTROPY" and entry.out_type == 2 and self.entropy_stime > entry.stime):
            self.entropy_stime = entry.stime
        elif (entry.proc == "ENCDEC" and entry.out_type == -1):
            self.encdec_cputime += entry.duration
            if (entry.stime < self.encdec_stime):
                self.encdec_stime = entry.stime
            if (entry.etime > self.encdec_etime):
                self.encdec_etime = entry.etime
            # return
        elif (entry.proc == "ENCDEC" and entry.out_type == 0):
            return
            # self.encdec_cputime += entry.duration
        self.cpu_time += entry.duration
        return
    def get_latency(self):
        return (self.end_time - self.start_time)
    def get_cputime(self):
        return self.cpu_time
    def get_encdec_latency(self):
        return (self.encdec_etime - self.encdec_stime)
    def get_encdec_cputime(self):
        return self.encdec_cputime
    def get_encdec_cputime_persent(self):
        return self.encdec_cputime / self.cpu_time
    def get_me_latency(self):
        return (self.me_etime - self.me_stime)
    def get_me_cputime(self):
        return self.me_cputime
    def get_me_cputime_persent(self):
        return self.me_cputime / self.cpu_time
    def get_me_encdec_cputime_persent(self):
        return (self.me_cputime + self.encdec_cputime) / self.cpu_time
    def get_resource_latency(self):
        return self.resource_latency
    def get_pa_latency(self):
        return self.pa_latency
    def get_entropy_latency(self):
        return (self.entropy_etime - self.entropy_stime)

def main():
    if (len(sys.argv) != 3):
        print("usage: cmd + raw.csv + frame_count")
    raw_file = sys.argv[1]
    frames = int(sys.argv[2])
    frame_list = []
    frame_latency= []
    for i in range(0, frames):
        frame_list.append(FrameInfo(i))
    with open(raw_file, "rt") as in_file:
        next(in_file)
        for line in in_file:
            entry = TimeEntry(line)
            frame_list[entry.pic_num].process_entry(entry)

    print("POC, latency, RESOURCE, PA,    ME, ENCDEC, ENTROPY| cpu_time, enc_cpu, enc%, me_cpu, me%, me+enc%")
    for i in range(0, frames):
        frame_latency.append(frame_list[i].get_latency())
        print("{:3d}, {:6.2f}, {:6.2f}, {:6.2f}, {:6.2f}, {:6.2f}, {:5.2f}, {:7.2f}, {:7.2f}, {:.2%}, {:6.2f}, {:.2%}, {:.2%}".format(
            i, frame_list[i].get_latency(), frame_list[i].get_resource_latency(), frame_list[i].get_pa_latency(),
            frame_list[i].get_me_latency(), frame_list[i].get_encdec_latency(), frame_list[i].get_entropy_latency(),
            frame_list[i].get_cputime(),
            frame_list[i].get_encdec_cputime(), frame_list[i].get_encdec_cputime_persent(),
            frame_list[i].get_me_cputime(), frame_list[i].get_me_cputime_persent(),frame_list[i].get_me_encdec_cputime_persent()))
    print("average latency: {:7.2f}, max latency: {:7.2f}".format(sum(frame_latency) / frames, max(frame_latency)))

if __name__ == '__main__':
    main()
