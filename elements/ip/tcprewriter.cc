/*
 * tcprewriter.{cc,hh} -- rewrites packet source and destination
 * Max Poletto, Eddie Kohler
 *
 * Copyright (c) 2000 Massachusetts Institute of Technology.
 *
 * This software is being provided by the copyright holders under the GNU
 * General Public License, either version 2 or, at your discretion, any later
 * version. For more information, see the `COPYRIGHT' file in the source
 * distribution.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif
#include "tcprewriter.hh"
#include "click_ip.h"
#include "click_tcp.h"
#include "confparse.hh"
#include "straccum.hh"
#include "error.hh"

#include <limits.h>

// TCPMapping

TCPRewriter::TCPMapping::TCPMapping()
  : _seqno_delta(0), _ackno_delta(0)
{
}

void
TCPRewriter::TCPMapping::change_udp_csum_delta(unsigned old_word, unsigned new_word)
{
  const unsigned short *source_words = (const unsigned short *)&old_word;
  const unsigned short *dest_words = (const unsigned short *)&new_word;
  unsigned delta = _udp_csum_delta;
  for (int i = 0; i < 2; i++) {
    delta += (~ntohs(source_words[i]) & 0xFFFF);
    delta += ntohs(dest_words[i]);
  }
  if ((new_word & 0x80000000) && !(old_word & 0x80000000))
    delta--;
  else if (!(new_word & 0x80000000) && (old_word & 0x80000000))
    delta++;
  while (delta >> 16)
    delta = (delta & 0xFFFF) + (delta >> 16);
  _udp_csum_delta = delta;
  click_chatter("%x %x", ntohl(old_word), ntohl(new_word));
}

void
TCPRewriter::TCPMapping::apply(WritablePacket *p)
{
  click_ip *iph = p->ip_header();
  assert(iph);
  
  // IP header
  iph->ip_src = _mapto.saddr();
  iph->ip_dst = _mapto.daddr();

  unsigned sum = (~ntohs(iph->ip_sum) & 0xFFFF) + _ip_csum_delta;
  if (sum >> 16)
    sum = (sum & 0xFFFF) + (sum >> 16);
  iph->ip_sum = ~htons(sum);

  // TCP header
  click_tcp *tcph = reinterpret_cast<click_tcp *>(p->transport_header());
  tcph->th_sport = _mapto.sport();
  tcph->th_dport = _mapto.dport();

  // update sequence numbers
  unsigned short csum_delta = _udp_csum_delta;
  if (_seqno_delta)
    tcph->th_seq = htonl(ntohl(tcph->th_seq) + _seqno_delta);
  if (_ackno_delta)
    tcph->th_ack = htonl(ntohl(tcph->th_ack) + _ackno_delta);

  // update checksum
  unsigned sum2 = (~ntohs(tcph->th_sum) & 0xFFFF) + csum_delta;
  if (sum2 >> 16)
    sum2 = (sum2 & 0xFFFF) + (sum2 >> 16);
  tcph->th_sum = ~htons(sum2);
  
  mark_used();
}


// TCPRewriter

TCPRewriter::TCPRewriter()
  : _tcp_map(0), _timer(this)
{
}

TCPRewriter::~TCPRewriter()
{
  assert(!_timer.scheduled());
}

void *
TCPRewriter::cast(const char *n)
{
  if (strcmp(n, "IPRw") == 0)
    return (IPRw *)this;
  else if (strcmp(n, "TCPRewriter") == 0)
    return (TCPRewriter *)this;
  else
    return 0;
}

void
TCPRewriter::notify_noutputs(int n)
{
  set_noutputs(n < 1 ? 1 : n);
}

int
TCPRewriter::configure(const Vector<String> &conf, ErrorHandler *errh)
{
  if (conf.size() == 0)
    return errh->error("too few arguments; expected `TCPRewriter(INPUTSPEC, ...)'");
  set_ninputs(conf.size());

  int before = errh->nerrors();
  for (int i = 0; i < conf.size(); i++) {
    InputSpec is;
    if (parse_input_spec(conf[i], is, "input spec " + String(i), errh) >= 0)
      _input_specs.push_back(is);
  }
  return (errh->nerrors() == before ? 0 : -1);
}

int
TCPRewriter::initialize(ErrorHandler *errh)
{
  _timer.attach(this);
  _timer.schedule_after_ms(GC_INTERVAL_SEC * 1000);
#if defined(CLICK_LINUXMODULE) && !defined(HAVE_TCP_PROT)
  errh->message
    ("The kernel does not export the symbol `tcp_prot', so I cannot remove\n"
     "stale mappings. Apply the Click kernel patch to fix this problem.");
#endif
#ifndef CLICK_LINUXMODULE
  errh->message("can't remove stale mappings at userlevel");
#endif
  return 0;
}

void
TCPRewriter::uninitialize()
{
  _timer.unschedule();
  clear_map(_tcp_map);
  for (int i = 0; i < _input_specs.size(); i++)
    if (_input_specs[i].kind == INPUT_SPEC_PATTERN)
      _input_specs[i].u.pattern.p->unuse();
}

void
TCPRewriter::take_state(Element *e, ErrorHandler *errh)
{
  TCPRewriter *rw = (TCPRewriter *)e->cast("TCPRewriter");
  if (!rw) return;

  if (noutputs() != rw->noutputs()) {
    errh->warning("taking mappings from `%s', although it has\n%s output ports", rw->declaration().cc(), (rw->noutputs() > noutputs() ? "more" : "fewer"));
    if (noutputs() < rw->noutputs())
      errh->message("(out of range mappings will be dropped)");
  }

  _tcp_map.swap(rw->_tcp_map);

  // check rw->_all_patterns against our _all_patterns
  Vector<Pattern *> pattern_map;
  for (int i = 0; i < rw->_all_patterns.size(); i++) {
    Pattern *p = rw->_all_patterns[i], *q = 0;
    for (int j = 0; j < _all_patterns.size() && !q; j++)
      if (_all_patterns[j]->can_accept_from(*p))
	q = _all_patterns[j];
    pattern_map.push_back(q);
  }
  
  take_state_map(_tcp_map, rw->_all_patterns, pattern_map);
}

void
TCPRewriter::run_scheduled()
{
#if defined(CLICK_LINUXMODULE) && defined(HAVE_TCP_PROT)
  mark_live_tcp(_tcp_map);
#endif
  clean_map(_tcp_map);
  _timer.schedule_after_ms(GC_INTERVAL_SEC * 1000);
}

TCPRewriter::TCPMapping *
TCPRewriter::apply_pattern(Pattern *pattern, int fport, int rport,
			   bool tcp, const IPFlowID &flow)
{
  assert(fport >= 0 && fport < noutputs() && rport >= 0 && rport < noutputs()
	 && tcp);
  TCPMapping *forward = new TCPMapping;
  TCPMapping *reverse = new TCPMapping;
  if (forward && reverse
      && pattern->create_mapping(flow, fport, rport, forward, reverse)) {
    IPFlowID reverse_flow = forward->flow_id().rev();
    _tcp_map.insert(flow, forward);
    _tcp_map.insert(reverse_flow, reverse);
    return forward;
  } else {
    delete forward;
    delete reverse;
    return 0;
  }
}

void
TCPRewriter::push(int port, Packet *p_in)
{
  WritablePacket *p = p_in->uniqueify();
  IPFlowID flow(p);
  click_ip *iph = p->ip_header();
  assert(iph->ip_p == IP_PROTO_TCP);

  TCPMapping *m = static_cast<TCPMapping *>(_tcp_map.find(flow));
  
  if (!m) {			// create new mapping
    const InputSpec &is = _input_specs[port];
    switch (is.kind) {

     case INPUT_SPEC_NOCHANGE:
      output(is.u.output).push(p);
      return;

     case INPUT_SPEC_DROP:
      break;

     case INPUT_SPEC_PATTERN: {
       Pattern *pat = is.u.pattern.p;
       int fport = is.u.pattern.fport;
       int rport = is.u.pattern.rport;
       m = TCPRewriter::apply_pattern(pat, fport, rport, true, flow);
       m->update_seqno_delta(2);
       break;
     }

     case INPUT_SPEC_MAPPER: {
       m = static_cast<TCPMapping *>(is.u.mapper->get_map(this, true, flow));
       break;
     }
      
    }
    if (!m) {
      p->kill();
      return;
    }
  }
  
  m->apply(p);
  output(m->output()).push(p);
}


String
TCPRewriter::dump_mappings_handler(Element *e, void *)
{
  TCPRewriter *rw = (TCPRewriter *)e;
  StringAccum tcps;
  for (Map::Iterator iter = rw->_tcp_map.first(); iter; iter++) {
    Mapping *m = iter.value();
    if (!m->is_reverse())
      tcps << m->s() << "\n";
  }
  return tcps.take_string();
}

String
TCPRewriter::dump_patterns_handler(Element *e, void *)
{
  TCPRewriter *rw = (TCPRewriter *)e;
  String s;
  for (int i = 0; i < rw->_input_specs.size(); i++)
    if (rw->_input_specs[i].kind == INPUT_SPEC_PATTERN)
      s += rw->_input_specs[i].u.pattern.p->s() + "\n";
  return s;
}

void
TCPRewriter::add_handlers()
{
  add_read_handler("mappings", dump_mappings_handler, (void *)0);
  add_read_handler("patterns", dump_patterns_handler, (void *)0);
}

ELEMENT_REQUIRES(IPRw IPRewriterPatterns)
EXPORT_ELEMENT(TCPRewriter)
