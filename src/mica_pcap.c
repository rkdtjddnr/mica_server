

#define PCAP_VERSION_MAJOR 2
#define PCAP_VERSION_MINOR 4

#define IPV4_HDR_SIZE sizeof(struct rte_ipv4_hdr)
#define UDP_HDR_SIZE sizeof(struct rte_udp_hdr)

// TIME INTERVAL
#pragma pack(1)
struct pcap_timeval {
	bpf_u_int32 tv_sec;	/* seconds */
	bpf_u_int32 tv_usec;	/* microseconds */
};

// PACKET HEADER
struct pcap_sf_pkthdr {
	struct pcap_timeval ts;	/* time stamp */
	bpf_u_int32 caplen;	/* length of portion present */
	bpf_u_int32 len;	/* length of this packet (off wire) */
};
#pragma pack()


// GLOBAL HEADER
struct pcap_file_header {
	bpf_u_int32 magic;
	u_short version_major;
	u_short version_minor;
	bpf_int32 thiszone;	/* not used - SHOULD be filled with 0 */
	bpf_u_int32 sigfigs;	/* not used - SHOULD be filled with 0 */
	bpf_u_int32 snaplen;	/* max length saved portion of each pkt */
	bpf_u_int32 linktype;	/* data link type (LINKTYPE_*) */
};



// global header 구성하는 함수
static int
sf_write_header(FILE *fp, int linktype, int snaplen)
{
	struct pcap_file_header hdr;

	hdr.magic = p->opt.tstamp_precision == PCAP_TSTAMP_PRECISION_NANO ? NSEC_TCPDUMP_MAGIC : TCPDUMP_MAGIC;
	hdr.version_major = PCAP_VERSION_MAJOR;
	hdr.version_minor = PCAP_VERSION_MINOR;
	hdr.thiszone = 0;
	hdr.sigfigs = 0;
	hdr.snaplen = snaplen;
	hdr.linktype = linktype;

	if (fwrite((char *)&hdr, sizeof(hdr), 1, fp) != 1)
		return (-1);

	return (0);
}

void save_packet_to_pcap(struct rte_mbuf *mbuf, PKTTYPE pkttype)
{

}
