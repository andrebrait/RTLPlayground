// Throwaway mock-up: Ports page layouts that fold in EEE and flow control.
// Load after app.js, then call mock("A"|"B"|"C"|"D"). Not for merging.
LANG.en.nav_fc="Flow control";LANG.en.fc_title="Flow control (802.3x pause)";LANG.en.fc_neg="Negotiated";
var FC={}, EEE={};
function fakeData(){
  S.ports.forEach(function(p){if(p.link>0)p.link=p.isSFP?5:[6,6,3,2][p.portNum%4];if(!p.isSFP)p.adv=p.portNum===7?"011111":"111111"});
  // fc bits as in PR #652: 0 adv pause, 1 adv asym, 2-3 partner, 4 TX, 5 RX negotiated
  S.ports.forEach(function(p){
    var i=p.portNum, up=p.enabled&&p.link>0;
    FC[i]=p.isSFP?(up?0x30:0):(i===4?0:(1|(up?(i===3?0x08:0x34):0)));
    EEE[i]={adv:p.isSFP?"000":(i===2?"000":"111"),lp:up&&!p.isSFP?"110":"000",active:up&&!p.isSFP&&i!==3};
  });
}
function sw(on,dis){
  var l=h("label",{class:"switch"},[h("input",{type:"checkbox"}),h("i")]);
  l.firstChild.checked=!!on;if(dis)l.firstChild.disabled=true;
  return l;
}
function fcNeg(p){var n=FC[p.portNum]>>4&3;return n?badge(["","TX","RX","TX/RX"][n],"ok"):badge(t("c_off"))}
function eeeSt(p){var e=EEE[p.portNum];return p.isSFP?badge(t("e_na")):e.active?badge(t("e_active"),"ok"):badge(t("e_idle"))}
function cellWith(tr,kids){var c=tr.insertCell();c.style.whiteSpace="nowrap";kids.forEach(function(k){typeof k==="string"?c.insertAdjacentHTML("beforeend",k):c.appendChild(k)});return c}

function baseRow(tb,p,opt){
  var i=p.portNum-1;
  var spd=h("select",{class:"in",id:"pspd"+i});
  (p.isSFP?SFPRATES:SPEEDS).forEach(function(o){spd.appendChild(h("option",{value:o[0],text:speedLabel(o[1])}))});
  var tr=tb.insertRow();
  tr.insertCell().textContent=p.portNum+(p.isSFP?" (SFP)":"");
  tr.insertCell().appendChild(h("input",{class:"in",id:"pname"+i,size:"9",maxlength:"15",value:p.name||"",placeholder:"-"}));
  var lk=tr.insertCell();lk.id="plink"+i;lk.style.whiteSpace="nowrap";
  var mc=tr.insertCell();mc.id="pmac"+i;mc.className="mono";mc.textContent="-";
  tr.insertCell().appendChild(spd);
  var e=sw(p.enabled);e.firstChild.id="pen"+i;tr.insertCell().appendChild(e);
  tr.insertCell().appendChild(h("input",{class:"in sm",id:"pmtu"+i,type:"number",min:"64",max:"16383"}));
  if(opt&&opt.before)opt.before(tr,p);
  tr.insertCell().appendChild(h("button",{class:"ctl",text:t("c_apply")}));
  return tr;
}
function setHead(cells,extra){
  var th=$("ptable").tHead;th.innerHTML="";
  var r=th.insertRow();
  cells.forEach(function(c){var e=h("th",{text:c[0]});if(c[1])for(var k in c[1])e.setAttribute(k,c[1][k]);r.appendChild(e)});
  if(extra){var r2=th.insertRow();extra.forEach(function(c){r2.appendChild(h("th",{text:c}))})}
}
var BASE=[t("c_port"),t("c_name"),t("c_link"),t("pt_conn"),t("pt_speed"),t("c_enabled"),t("pt_mtu")];
function clearExtras(){
  var sec=$("ptable").closest("section");
  sec.querySelectorAll(".mockx").forEach(function(e){e.remove()});
  $("ptable").tBodies[0].innerHTML="";
}

var VARIANTS={
  // A: one column per feature: switch plus live state, advertised/partner flags underneath
  A:function(){
    setHead(BASE.map(function(s){return[s]}).concat([[t("nav_fc")],[t("nav_eee")],[""]]));
    function sub(a,b){return h("div",{class:"small mut",style:"margin-top:3px;line-height:1.35",html:esc(t("e_adv"))+": "+esc(a)+"<br>"+esc(t("e_lp"))+": "+esc(b)})}
    S.ports.forEach(function(p){
      var f=FC[p.portNum],e=EEE[p.portNum];
      baseRow($("ptable").tBodies[0],p,{before:function(tr){
        var c=cellWith(tr,p.isSFP?[badge(t("e_na"))," ",fcNeg(p)]:[sw(f&1)," ",fcNeg(p)]);
        if(!p.isSFP)c.appendChild(sub(fcFlags(f&3),fcFlags(f>>2&3)));
        c=cellWith(tr,p.isSFP?[badge(t("e_na"))]:[sw(parseInt(e.adv,2))," ",eeeSt(p)]);
        if(!p.isSFP)c.appendChild(sub(eeeFlags(e.adv),eeeFlags(e.lp)));
      }});
    });
  },
  // B: grouped headers, every flag in its own column (both current pages side by side)
  B:function(){
    var g={colspan:4,style:"text-align:center;border-bottom:0;border-left:1px solid var(--bd)"};
    setHead(BASE.map(function(s){return[s,{rowspan:2}]}).concat([[t("nav_fc"),g],[t("nav_eee"),g],["",{rowspan:2}]]),
      [t("e_adv"),t("e_lp"),t("fc_neg"),t("c_enabled"),t("e_adv"),t("e_lp"),t("e_active"),t("c_enabled")]);
    var r2=$("ptable").tHead.rows[1].cells;r2[0].style.borderLeft=r2[4].style.borderLeft="1px solid var(--bd)";
    S.ports.forEach(function(p){
      var f=FC[p.portNum],e=EEE[p.portNum],dash=function(){return h("span",{class:"mut",text:"-"})};
      baseRow($("ptable").tBodies[0],p,{before:function(tr){
        cellWith(tr,[p.isSFP?"-":fcFlags(f&3)]).style.borderLeft="1px solid var(--bd)";
        cellWith(tr,[p.isSFP?"-":fcFlags(f>>2&3)]);
        cellWith(tr,[fcNeg(p)]);
        cellWith(tr,[p.isSFP?dash():sw(f&1)]);
        cellWith(tr,[p.isSFP?"-":eeeFlags(e.adv)]).style.borderLeft="1px solid var(--bd)";
        cellWith(tr,[p.isSFP?"-":eeeFlags(e.lp)]);
        cellWith(tr,[eeeSt(p)]);
        cellWith(tr,[p.isSFP?dash():sw(parseInt(e.adv,2))]);
      }});
    });
  },
  // E: one card per port instead of a table row; aimed at phones
  E:function(){
    setHead(BASE.map(function(s){return[s]}).concat([[""]]));
    var wrap=$("ptable").closest(".scrollx");wrap.style.display="none";
    var list=h("div",{class:"mockx",style:"display:grid;grid-template-columns:repeat(auto-fill,minmax(300px,1fr));gap:10px"});
    function kv(k,v){return[h("span",{class:"mut small",text:k}),typeof v==="string"?h("span",{html:v}):v]}
    function grid(rows,cols){var g=h("div",{style:"display:grid;grid-template-columns:"+(cols||"auto 1fr")+";gap:6px 10px;align-items:center"});rows.forEach(function(r){kv(r[0],r[1]).forEach(function(e){g.appendChild(e)})});return g}
    function sec(title,on,state,rows,sfp){
      var body=h("div",{style:"line-height:1.35"});
      rows.forEach(function(r){body.appendChild(h("div",{class:"mut small",text:r[0]}));body.appendChild(h("div",{style:"margin-bottom:4px",text:r[1]}))});
      return h("div",{style:"min-width:0"},[
        h("div",{style:"display:flex;gap:6px;align-items:center;flex-wrap:wrap;font-weight:600;margin-bottom:4px"},[sfp?h("span",{html:badge(t("e_na"))}):sw(on),h("span",{text:title}),h("span",{html:state})]),
        body]);
    }
    S.ports.forEach(function(p){
      var i=p.portNum-1,f=FC[p.portNum],e=EEE[p.portNum];
      var spd=h("select",{class:"in"});
      (p.isSFP?SFPRATES:SPEEDS).forEach(function(o){spd.appendChild(h("option",{value:o[0],text:speedLabel(o[1])}))});
      var mtu=h("input",{class:"in",type:"number",style:"width:100%;box-sizing:border-box",value:i%2?"1522":"16383"});
      spd.style.width="100%";
      list.appendChild(h("div",{style:"border:1px solid var(--bd);border-radius:8px;padding:10px 12px"},[
        h("div",{style:"display:flex;gap:8px;align-items:center;margin-bottom:8px"},[
          h("b",{style:"white-space:nowrap",text:t("c_port")+" "+p.portNum+(p.isSFP?" (SFP)":"")}),h("span",{html:linkBadge(p)}),
          h("span",{class:"mut small",text:t("pt_conn")+": "+(parseInt(p.macs,16)&0x1fff)}),
          h("span",{style:"flex:1"}),sw(p.enabled),h("button",{class:"ctl",text:t("c_apply")})]),
        grid([[t("c_name"),h("input",{class:"in",style:"width:100%;box-sizing:border-box",placeholder:"-"})],[t("pt_speed"),spd],[t("pt_mtu"),mtu]]),
        h("div",{style:"display:grid;grid-template-columns:1fr 1fr;gap:12px;border-top:1px solid var(--bd);padding-top:8px;margin-top:8px"},[
          sec(t("nav_fc"),f&1,fcNeg(p),[[t("e_adv"),p.isSFP?"-":fcFlags(f&3)],[t("e_lp"),p.isSFP?"-":fcFlags(f>>2&3)]],p.isSFP),
          sec(t("nav_eee"),parseInt(e.adv,2),eeeSt(p),[[t("e_adv"),p.isSFP?"-":eeeFlags(e.adv)],[t("e_lp"),p.isSFP?"-":eeeFlags(e.lp)]],p.isSFP)]),
      ]));
    });
    wrap.after(list);
  },
  // C: Ports page keeps its table; a second card on the same page merges the EEE and flow control pages
  C:function(){
    setHead(BASE.map(function(s){return[s]}).concat([[""]]));
    S.ports.forEach(function(p){baseRow($("ptable").tBodies[0],p)});
    var tbl=h("table",{class:"t"});
    var th=tbl.createTHead(),r=th.insertRow(),r2=th.insertRow();
    [[t("c_port"),{rowspan:2}],[t("nav_fc"),{colspan:4}],[t("nav_eee"),{colspan:4}]].forEach(function(c){
      var e=h("th",{text:c[0]});for(var k in c[1])e.setAttribute(k,c[1][k]);if(c[1].colspan)e.style.cssText="text-align:center;border-left:1px solid var(--bd);border-bottom:0";r.appendChild(e);
    });
    [t("e_adv"),t("e_lp"),t("fc_neg"),t("c_enabled"),t("e_adv"),t("e_lp"),t("e_active"),t("c_enabled")].forEach(function(s,i){
      var e=h("th",{text:s});if(i%4===0)e.style.borderLeft="1px solid var(--bd)";r2.appendChild(e);
    });
    var tb=tbl.createTBody();
    S.ports.forEach(function(p){
      var tr=tb.insertRow(),f=FC[p.portNum],e=EEE[p.portNum];
      tr.insertCell().textContent=p.portNum+(p.isSFP?" (SFP)":"");
      var c=tr.insertCell();c.style.borderLeft="1px solid var(--bd)";c.textContent=p.isSFP?"-":fcFlags(f&3);
      tr.insertCell().textContent=p.isSFP?"-":fcFlags(f>>2&3);
      cellWith(tr,[fcNeg(p)]);
      cellWith(tr,[p.isSFP?h("span",{class:"mut",text:t("e_na")}):sw(f&1)]);
      c=tr.insertCell();c.style.borderLeft="1px solid var(--bd)";c.textContent=p.isSFP?"-":eeeFlags(e.adv);
      tr.insertCell().textContent=p.isSFP?"-":eeeFlags(e.lp);
      cellWith(tr,[eeeSt(p)]);
      cellWith(tr,[p.isSFP?h("span",{class:"mut",text:t("e_na")}):sw(parseInt(e.adv,2))]);
    });
    var card=h("div",{class:"card mockx"},[h("h2",{html:esc(t("nav_fc"))+" &amp; "+esc(t("nav_eee"))+'<span class="hint">'+esc(t("fc_title"))+", 802.3az</span>"}),h("div",{class:"scrollx"},[tbl]),
      h("p",{class:"small mut",style:"margin-top:8px",text:"Toggles apply immediately and renegotiate the link."})]);
    $("ptable").closest(".card").after(card);
  },
  // D: one row per port as today, plus an expandable detail row with both features
  D:function(){
    setHead([[""]].concat(BASE.map(function(s){return[s]})).concat([[""]]));
    S.ports.forEach(function(p,idx){
      var tb=$("ptable").tBodies[0],tr=baseRow(tb,p);
      var open=idx===0||idx===3;
      var tg=tr.insertCell(0);tg.appendChild(h("button",{class:"ctl",text:open?"\u25bc":"\u25b6",style:"padding:2px 7px;font-size:11px"}));
      if(!open)return;
      var dr=tb.insertRow(),dc=dr.insertCell();dc.colSpan=tr.cells.length;dc.style.background="var(--panel2)";
      var f=FC[p.portNum],e=EEE[p.portNum];
      function block(title,on,rows,state){
        var g=h("div",{style:"display:grid;grid-template-columns:auto auto;gap:4px 14px;align-items:center"});
        rows.forEach(function(r){g.appendChild(h("span",{class:"mut small",text:r[0]}));g.appendChild(h("span",{text:r[1]}))});
        return h("div",{style:"min-width:260px"},[
          h("div",{style:"display:flex;gap:10px;align-items:center;margin-bottom:6px;font-weight:600"},[sw(on),h("span",{text:title}),h("span",{html:state})]),g]);
      }
      dc.appendChild(h("div",{style:"display:flex;gap:40px;flex-wrap:wrap;padding:4px 0 4px 36px"},[
        block(t("nav_fc"),f&1,[[t("e_adv"),fcFlags(f&3)],[t("e_lp"),fcFlags(f>>2&3)]],fcNeg(p)),
        block(t("nav_eee"),parseInt(e.adv,2),[[t("e_adv"),eeeFlags(e.adv)],[t("e_lp"),eeeFlags(e.lp)]],eeeSt(p)),
      ]));
    });
  },
};

// Final round (PR #652 feedback): overview switch + state, details behind an Advanced control
function advSpeeds(p){
  var b=parseInt(p.adv||"0",2),n=["10M "+t("c_half"),"10M "+t("c_full"),"100M "+t("c_half"),"100M "+t("c_full"),"1G","2.5G"];
  return n.filter(function(_,i){return b&(1<<i)}).reverse().join(", ")||"-";
}
function fcSw(p){return p.isSFP?h("span",{html:badge(t("e_na"))}):sw(FC[p.portNum]&1)}
function eeeSw(p){return p.isSFP?h("span",{html:badge(t("e_na"))}):sw(parseInt(EEE[p.portNum].adv,2))}
function feat(p){
  var f=FC[p.portNum],e=EEE[p.portNum];
  return[
    {name:t("nav_fc"),sw:fcSw(p),st:fcNeg(p),adv:p.isSFP?"-":fcFlags(f&3),lp:p.isSFP?"-":fcFlags(f>>2&3)},
    {name:t("nav_eee"),sw:eeeSw(p),st:eeeSt(p),adv:p.isSFP?"-":eeeFlags(e.adv),lp:p.isSFP?"-":eeeFlags(e.lp)},
  ];
}
// Desktop details: one row per feature, labels in the header
function detailsTable(p,withSw){
  var tb=h("table",{class:"t",style:"width:auto"}),r=tb.createTHead().insertRow();
  [""].concat(withSw?[t("c_enabled")]:[]).concat(["Status",t("e_adv"),t("e_lp")]).forEach(function(s){r.appendChild(h("th",{text:s}))});
  var b=tb.createTBody();
  feat(p).forEach(function(x){
    var tr=b.insertRow();
    var c=tr.insertCell();c.style.fontWeight="600";c.textContent=x.name;
    if(withSw){c=tr.insertCell();if(x.sw)c.appendChild(x.sw)}
    cellWith(tr,[x.st]);
    tr.insertCell().textContent=x.adv;tr.insertCell().textContent=x.lp;
  });
  return tb;
}
// Phone details: each feature is a row (switch, name, state), with label/value pairs side by side underneath
function detailsList(p,withSw){
  var d=h("div");
  feat(p).forEach(function(x,i){
    var g=h("div",{style:"display:grid;grid-template-columns:7.5em 1fr;gap:4px 10px;margin:6px 0 0"});
    [[t("e_adv"),x.adv]].concat(x.lp!==""?[[t("e_lp"),x.lp]]:[]).forEach(function(kv){
      g.appendChild(h("span",{class:"mut small",text:kv[0]}));g.appendChild(h("span",{text:kv[1]}));
    });
    d.appendChild(h("div",{style:"padding:8px 0;"+(i?"border-top:1px solid var(--bd)":"")},[
      h("div",{style:"display:flex;gap:8px;align-items:center"},[h("b",{text:x.name}),h("span",{html:x.st}),h("span",{style:"flex:1"})].concat(withSw&&x.sw?[x.sw]:[])),g]));
  });
  return d;
}
function toggleBtn(open,label){return h("button",{class:"ctl",style:"white-space:nowrap",text:(label?label+" ":"")+(open?"\u25b4":"\u25be")})}
function advBtn(p){return h("button",{class:"ctl",text:"Advanced\u2026",onclick:function(){modal(t("c_port")+" "+p.portNum+": Advanced",detailsList(p,true),[h("button",{class:"pri",text:t("c_apply")})])}})}
// Phone card: one row per setting, label left of value
function card(p,extra){
  var spd=h("select",{class:"in",style:"width:100%"});
  (p.isSFP?SFPRATES:SPEEDS).forEach(function(o){spd.appendChild(h("option",{value:o[0],text:speedLabel(o[1])}))});
  var g=h("div",{style:"display:grid;grid-template-columns:7.5em 1fr;gap:6px 10px;align-items:center"});
  [[t("c_name"),h("input",{class:"in",style:"width:100%;box-sizing:border-box",placeholder:"-"})],[t("pt_speed"),spd],
   [t("pt_mtu"),h("input",{class:"in",type:"number",style:"width:100%;box-sizing:border-box",value:p.portNum%2?"16383":"1522"})],
   [t("pt_conn"),h("span",{text:String(parseInt(p.macs,16)&0x1fff)})]].forEach(function(kv){
    g.appendChild(h("span",{class:"mut small",text:kv[0]}));g.appendChild(kv[1]);
  });
  return h("div",{style:"border:1px solid var(--bd);border-radius:8px;padding:10px 12px"},[
    h("div",{style:"display:flex;gap:8px;align-items:center;margin-bottom:8px"},[
      h("b",{style:"white-space:nowrap",text:t("c_port")+" "+p.portNum+(p.isSFP?" (SFP)":"")}),h("span",{style:"white-space:nowrap"},[h("span",{html:linkBadge(p)}),infoBtn(p)]),
      h("span",{style:"flex:1"}),sw(p.enabled),h("button",{class:"ctl",text:t("c_apply")})]),g].concat(extra));
}
function cardList(fn){
  var wrap=$("ptable").closest(".scrollx");wrap.style.display="none";
  var list=h("div",{class:"mockx",style:"display:grid;grid-template-columns:repeat(auto-fill,minmax(300px,1fr));gap:10px"});
  S.ports.forEach(function(p,i){list.appendChild(card(p,fn(p,i)))});
  wrap.after(list);
}
// F: overview columns (switch + state); a per-port toggle expands a details row
VARIANTS.F=function(){
  setHead([[""]].concat(BASE.map(function(s){return[s]})).concat([[t("nav_fc")],[t("nav_eee")],[""]]));
  S.ports.forEach(function(p,idx){
    var tb=$("ptable").tBodies[0],open=idx===0||idx===S.ports.length-1;
    var tr=baseRow(tb,p,{before:function(tr){cellWith(tr,[fcSw(p)," ",fcNeg(p)]);cellWith(tr,[eeeSw(p)," ",eeeSt(p)])}});
    tr.insertCell(0).appendChild(toggleBtn(open));
    if(!open)return;
    var dc=tb.insertRow().insertCell();dc.colSpan=tr.cells.length;dc.style.cssText="background:var(--panel2);padding:6px 8px 10px 52px";
    dc.appendChild(detailsTable(p,false));
  });
};
// G: no extra columns; an Advanced button per port opens a modal with switches and details
VARIANTS.G=function(){
  setHead(BASE.map(function(s){return[s]}).concat([[""],[""]]));
  S.ports.forEach(function(p){baseRow($("ptable").tBodies[0],p,{before:function(tr){tr.insertCell().appendChild(advBtn(p))}})});
  modal(t("c_port")+" 1: Advanced",(function(){var d=h("div");d.appendChild(detailsTable(S.ports[0],true));return d})(),[h("button",{class:"pri",text:t("c_apply")})]);
};
// M: phone version of F: flow control and EEE as rows, details expand in the card
VARIANTS.M=function(){
  cardList(function(p,i){
    var rows=[[t("nav_fc"),fcSw(p),fcNeg(p)],[t("nav_eee"),eeeSw(p),eeeSt(p)]].map(function(r){
      return h("div",{style:"display:flex;gap:8px;align-items:center;padding:6px 0;border-top:1px solid var(--bd)"},[h("span",{text:r[0]}),h("span",{html:r[2]}),h("span",{style:"flex:1"}),r[1]]);
    });
    rows[0].style.marginTop="8px";
    var open=i===0,more=h("div",{style:"border-top:1px solid var(--bd);padding-top:6px"},[toggleBtn(open,"Advanced")]);
    if(open)more.appendChild(h("div",{style:"background:var(--panel2);border-radius:6px;padding:0 10px;margin-top:6px"},[detailsList(p,false)]));
    return rows.concat([more]);
  });
};
// N: phone version of G: Advanced button opens the modal
VARIANTS.N=function(){
  cardList(function(p){return[h("div",{style:"border-top:1px solid var(--bd);padding-top:8px;margin-top:8px"},[advBtn(p)])]});
  modal(t("c_port")+" 1: Advanced",detailsList(S.ports[0],true),[h("button",{class:"pri",text:t("c_apply")})]);
};
// Link speed details: separate from flow control/EEE, behind an info button next to the link badge
var LINKINFO=false;
function linkTable(p){
  var d=h("div"),up=p.enabled&&p.link>0,pc="var("+(LINKC[p.link]||"--s1000")+")";
  if(p.isSFP){
    d.appendChild(h("p",{class:"small mut",style:"max-width:16em",text:"SFP ports do not advertise speeds."}));
    return d;
  }
  // one row per speed, like the port strip: duplex is not shown; adv bits 0-1 = 10M, 2-3 = 100M, 4 = 1G, 5 = 2.5G
  var a=parseInt(p.adv||"0",2),n=["10M","100M","1G","2.5G"],b=[a&3,a&12,a&16,a&32],link={1:0,2:1,3:2,6:3};
  var tb=h("table",{class:"t",style:"width:auto"}),r=tb.createTHead().insertRow();
  [t("pt_speed"),t("e_adv")].forEach(function(s){r.appendChild(h("th",{text:s}))});
  var bd=tb.createTBody(),cur=up?link[p.link]:-1;
  for(var i=n.length-1;i>=0;i--){
    var tr=bd.insertRow(),c0=tr.insertCell(),c1=tr.insertCell();
    c0.textContent=n[i];
    if(i===cur){
      // same look as the port strip tile for this link speed
      c0.style.cssText="font-weight:650;color:"+pc;
      c1.innerHTML='<span class="badge" title="Current link" style="color:'+pc+';border:1px solid '+pc+';background:color-mix(in srgb,'+pc+' 10%,var(--panel2))">\u2713 '+esc(t("c_link"))+'</span>';
    }else c1.innerHTML=b[i]?badge("\u2713","ok"):'<span class="mut">-</span>';
  }
  d.appendChild(tb);
  return d;
}
var _modal=modal;
modal=function(a,b,c){$("modal").style.width="";_modal(a,b,c)};
function linkModal(p){modal(t("c_port")+" "+p.portNum+": "+t("c_link"),linkTable(p));$("modal").style.width="fit-content"}
function infoBtn(p){
  return h("button",{title:"Link details",style:"background:none;border:0;padding:0 0 0 6px;cursor:pointer;color:var(--ac);font-size:15px;vertical-align:middle",text:"\u24d8",
    onclick:function(){linkModal(p)}});
}
// H: Advanced button (as in G), but it expands the row instead of opening a modal
VARIANTS.H=function(){
  setHead(BASE.map(function(s){return[s]}).concat([[""],[""]]));
  S.ports.forEach(function(p,idx){
    var tb=$("ptable").tBodies[0],open=idx===0||idx===S.ports.length-1;
    var tr=baseRow(tb,p,{before:function(tr){tr.insertCell().appendChild(toggleBtn(open,"Advanced"))}});
    if(!open)return;
    var dc=tb.insertRow().insertCell();dc.colSpan=tr.cells.length;dc.style.cssText="background:var(--panel2);padding:6px 8px 10px 52px";
    dc.appendChild(detailsTable(p,true));
  });
};
// HM: phone version of H: the Advanced button expands the card
VARIANTS.HM=function(){
  cardList(function(p,i){
    var open=i===0,more=h("div",{style:"border-top:1px solid var(--bd);padding-top:8px;margin-top:8px"},[toggleBtn(open,"Advanced")]);
    if(open)more.appendChild(h("div",{style:"background:var(--panel2);border-radius:6px;padding:0 10px;margin-top:6px"},[detailsList(p,true)]));
    return[more];
  });
};
// G2/H2: G and H plus read-only status columns; switches and flags stay behind Advanced
function statusHead(){setHead(BASE.map(function(s){return[s]}).concat([[t("nav_fc")],[t("nav_eee")],[""],[""]]))}
function statusCells(tr,p){cellWith(tr,[fcNeg(p)]);cellWith(tr,[eeeSt(p)])}
VARIANTS.G2=function(){
  statusHead();
  S.ports.forEach(function(p){baseRow($("ptable").tBodies[0],p,{before:function(tr){statusCells(tr,p);tr.insertCell().appendChild(advBtn(p))}})});
  modal(t("c_port")+" 1: Advanced",(function(){var d=h("div");d.appendChild(detailsTable(S.ports[0],true));return d})(),[h("button",{class:"pri",text:t("c_apply")})]);
};
VARIANTS.H2=function(){
  statusHead();
  S.ports.forEach(function(p,idx){
    var tb=$("ptable").tBodies[0],open=idx===0||idx===S.ports.length-1;
    var tr=baseRow(tb,p,{before:function(tr){statusCells(tr,p);tr.insertCell().appendChild(toggleBtn(open,"Advanced"))}});
    if(!open)return;
    var dc=tb.insertRow().insertCell();dc.colSpan=tr.cells.length;dc.style.cssText="background:var(--panel2);padding:6px 8px 10px 52px";
    dc.appendChild(detailsTable(p,true));
  });
};
function statusRows(p){
  return[[t("nav_fc"),fcNeg(p)],[t("nav_eee"),eeeSt(p)]].map(function(r,i){
    return h("div",{style:"display:grid;grid-template-columns:7.5em 1fr;gap:10px;align-items:center;padding:4px 0"+(i?"":";margin-top:6px")},[h("span",{class:"mut small",text:r[0]}),h("span",{html:r[1]})]);
  });
}
VARIANTS.G2M=function(){
  cardList(function(p){return statusRows(p).concat([h("div",{style:"border-top:1px solid var(--bd);padding-top:8px;margin-top:6px"},[advBtn(p)])])});
  modal(t("c_port")+" 1: Advanced",detailsList(S.ports[0],true),[h("button",{class:"pri",text:t("c_apply")})]);
};
VARIANTS.H2M=function(){
  cardList(function(p,i){
    var open=i===0,more=h("div",{style:"border-top:1px solid var(--bd);padding-top:8px;margin-top:6px"},[toggleBtn(open,"Advanced")]);
    if(open)more.appendChild(h("div",{style:"background:var(--panel2);border-radius:6px;padding:0 10px;margin-top:6px"},[detailsList(p,true)]));
    return statusRows(p).concat([more]);
  });
};
["F","G","H","M","N","HM","G2","H2","G2M","H2M"].forEach(function(v){var f=VARIANTS[v];VARIANTS[v]=function(){LINKINFO=true;f()}});
VARIANTS.G2L=function(){VARIANTS.G2();linkModal(S.ports[2])};
VARIANTS.G2ML=function(){VARIANTS.G2M();linkModal(S.ports[0])};
VARIANTS.G2C=function(){VARIANTS.G2();closeModal()};
VARIANTS.G2MC=function(){VARIANTS.G2M();closeModal()};
VARIANTS.FL=function(){VARIANTS.F();linkModal(S.ports[0])};
VARIANTS.ML=function(){VARIANTS.M();linkModal(S.ports[0])};
VARIANTS.FL3=function(){VARIANTS.F();linkModal(S.ports[2])};
function fcFlags(b){return["Pause","Asym"].filter(function(s,i){return b&(1<<i)}).join(", ")||"-"}
function mockStatus(){
  fakeData();updateStrip();
  S.ports.forEach(function(p){
    var i=p.portNum-1,el=$("plink"+i),mc=$("pmac"+i);
    if(mc)mc.textContent=parseInt(p.macs,16)&0x1fff;
    if(el){el.innerHTML=linkBadge(p);if(LINKINFO)el.appendChild(infoBtn(p))}
  });
}
function mock(v){
  fakeData();clearExtras();VARIANTS[v]();mockStatus();loadMtu();
  tabHooks.ports.status=mockStatus;
  buildPorts=function(){};
}
