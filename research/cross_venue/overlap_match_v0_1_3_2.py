"""High-recall structured + lexical market matching; no liquidity/economic filters."""
import argparse,hashlib,heapq,json,math,re,sqlite3,unicodedata
from collections import Counter,defaultdict
from datetime import datetime
from decimal import Decimal
from pathlib import Path
from overlap_collect_v0_1_3_2 import write
STOP=set('will the a an to of in be on for by or at and is yes no as from with than have has this market resolve resolves if then otherwise before after above below over under greater less more least most between any which who what when how does do did are was were it its there that during next end price win wins winner score points total game match season'.split())
MONTHS={m:i+1 for i,m in enumerate(['jan','feb','mar','apr','may','jun','jul','aug','sep','oct','nov','dec'])}
LEAGUES={'NCAAM':'ncaab','NCAAW':'ncaaw','NCAAF':'cfb','NBA':'nba','WNBA':'wnba','NFL':'nfl','NHL':'nhl','MLB':'mlb','EPL':'epl','EFLCHAMPIONSHIP':'efl','BUNDESLIGA':'bun','LALIGA':'lal','SERIEA':'sea','LIGUE1':'fl1','LIGAPORTUGAL':'por','MLS':'mls','UCL':'ucl','UFC':'ufc','ATP':'atp','WTA':'wta','F1':'f1'}
SYN={'bitcoin':'btc','ethereum':'eth','exceed':'above','exceeds':'above','surpass':'above','higher':'above','lower':'below','nominee':'nomination','nominated':'nomination','presidential':'president','championship':'champion','champions':'champion','united states':'us','united kingdom':'uk','new york city':'nyc','artificial intelligence':'ai','gubernatorial':'governor','democratic party':'democratic','republican party':'republican','first goalscorer':'first scorer','new york':'nyc'}
def norm(text):
    text=unicodedata.normalize('NFKD',str(text)).encode('ascii','ignore').decode().lower()
    text=re.sub(r'(\d[\d,]*(?:\.\d+)?)\s*([km])\b',lambda m:format(Decimal(m[1].replace(',',''))*(1000 if m[2]=='k' else 1000000),'f'),text)
    for a,b in SYN.items():text=re.sub(r'\b'+re.escape(a)+r'\b',b,text)
    text=re.sub(r'(?<=\d),(?=\d)','',text);return ' '.join(re.findall(r'[a-z0-9]+(?:\.[0-9]+)?',text))
def words(text):return set(w for w in norm(text).split() if w not in STOP and not re.fullmatch(r'\d{1,4}',w))
def arr(x):
    if isinstance(x,list):return x
    if isinstance(x,str):
        try:v=json.loads(x);return v if isinstance(v,list) else []
        except ValueError:return []
    return []
def outcome_ids(raw,partition):
    version=raw.get('version') or ('v2' if partition.endswith('v2') else 'v1');labels=raw.get('outcomes');out=[]
    if isinstance(labels,dict):
        for label,data in labels.items():
            value=data.get('positionId') if version=='v2' else data.get('tokenId');out.append({'label':label,'asset_id':str(value) if value is not None else None,'id_kind':'POSITION_ID' if version=='v2' else 'CTF_TOKEN_ID'})
    else:
        labels=arr(labels);ids=arr(raw.get('positionIds')) if version=='v2' else arr(raw.get('clobTokenIds'))
        for i,label in enumerate(labels):out.append({'label':str(label),'asset_id':str(ids[i]) if i<len(ids) and ids[i] is not None else None,'id_kind':'POSITION_ID' if version=='v2' else 'CTF_TOKEN_ID'})
    supported=version in ['v1','v2'] and len(out)>=2 and all(x['asset_id'] and re.fullmatch(r'[0-9]{1,78}',x['asset_id']) for x in out)
    return version,out,supported

def event_date(raw,venue):
    if venue=='kalshi':
        match=re.search(r'-(\d{2})(JAN|FEB|MAR|APR|MAY|JUN|JUL|AUG|SEP|OCT|NOV|DEC)(\d{2})',raw.get('event_ticker',''))
        if match:return f'20{match[1]}-{MONTHS[match[2].lower()]:02d}-{match[3]}','EVENT_TICKER_DATE_NOT_SETTLEMENT_TIME'
    else:
        for field in ['gameStartTime']:
            if raw.get(field) and re.match(r'\d{4}-\d\d-\d\d',str(raw[field])):return str(raw[field])[:10],field
        text=raw.get('slug','')+' '+str(raw.get('events',[{}])[0].get('slug','') if raw.get('events') else '')
        m=re.search(r'(20\d{2}-\d\d-\d\d)',text)
        if m:return m[1],'EVENT_SLUG_DATE_NOT_SETTLEMENT_TIME'
    if venue!='kalshi':
        text=str(raw.get('question',''))+' '+str(raw.get('slug',''))
        m=re.search(r'(?i)\b(jan(?:uary)?|feb(?:ruary)?|mar(?:ch)?|apr(?:il)?|may|jun(?:e)?|jul(?:y)?|aug(?:ust)?|sep(?:tember)?|oct(?:ober)?|nov(?:ember)?|dec(?:ember)?)[ -]+(\d{1,2})[, -]+(20\d{2})',text)
        if m:
            try:return datetime(int(m[3]),MONTHS[m[1][:3].lower()],int(m[2])).date().isoformat(),'EXPLICIT_QUESTION_OR_SLUG_CALENDAR_DATE'
            except ValueError:pass
    return None,None

TEAM_ALIASES={};TEAM_REGEX={}
def configure_teams(teams):
    byleague=defaultdict(lambda:defaultdict(set))
    for team in teams:
        league=team.get('league');name=norm(team.get('name',''));abbr=norm(team.get('abbreviation',''))
        if not name or not league:continue
        aliases={name,abbr};parts=name.split()
        if len(parts)>1:aliases.add(parts[-1]);aliases.add(' '.join(parts[:-1]))
        for alias in aliases:
            if len(alias)>=3:byleague[league][alias].add(name)
    for league,aliases in byleague.items():
        good={a:next(iter(names)) for a,names in aliases.items() if len(names)==1};TEAM_ALIASES[league]=good
        if good:TEAM_REGEX[league]=re.compile(r'\b(?:'+'|'.join(re.escape(a) for a in sorted(good,key=lambda a:(-len(a),a)))+r')\b')
def participants(league,text):
    if league not in TEAM_REGEX:return []
    return sorted({TEAM_ALIASES[league][m[0]] for m in TEAM_REGEX[league].finditer(norm(text))})

def canonical(record,events,series):
    raw=record['raw']
    if raw.get('mve_collection_ticker') or raw.get('mve_selected_legs'):
        title=raw.get('title','');rules=raw.get('rules_primary','')+'\n'+raw.get('rules_secondary','');identifier=record['id'];normalized=' '.join(re.findall(r'[a-z0-9]+',unicodedata.normalize('NFKD',title).encode('ascii','ignore').decode().lower()))
        return dict(venue='kalshi',uid='K:'+identifier,market_id=identifier,event_id=raw.get('event_ticker'),event_title=None,series_id=raw.get('mve_collection_ticker'),condition_id=None,request_id=None,outcome_ids=[{'label':'Yes','asset_id':'YES','id_kind':'KALSHI_SIDE'},{'label':'No','asset_id':'NO','id_kind':'KALSHI_SIDE'}],title=title,subtitle=raw.get('yes_sub_title'),normalized_title=normalized,event_normalized_title='',event_category='combo',category_source='MVE_MARKET_METADATA',entities=[],event_predicate='COMBO',outcome_definition=title,numerical_threshold=None,comparison_operator=None,reference_index=None,observation_timestamp=None,observation_window=None,event_timezone=None,sport_or_competition=None,participant_names=[],yes_outcome_entity=None,outcome_subject=None,event_date=None,event_date_evidence=None,years=[],period=None,resolution_source=None,source_rule_hash=hashlib.sha256(rules.encode()).hexdigest(),rules=rules,market_status=raw.get('status'),accepting_orders=None,protocol_version='KALSHI_MVE',reported_protocol_version=None,outcome_id_mapping_supported=True,existing_ws_adapter_supported=False,settlement_asset='USD',minimum_quantity=raw.get('minimum_order_size_fp'),quantity_step=raw.get('quantity_step_fp'),price_tick=None,price_ranges=raw.get('price_ranges'),close_time=raw.get('close_time'),expiration_time=raw.get('expiration_time'),source=record['source'],partition=record['partition'],url='https://kalshi.com/markets/'+identifier,slug=None,semantic_tokens=[],sports_market_type=None,neg_risk=None,composite_leg_semantic_matching_status='NOT_EXHAUSTIVE_RAW_LEG_REFERENCES_PRESERVED_IN_SOURCE')
    raw=record['raw'];venue=record['venue'];identifier=record['id'];is_k=venue=='kalshi';event=events.get(raw.get('event_ticker'),{}) if is_k else (raw.get('events') or [{}])[0];ser=series.get(event.get('series_ticker') or raw.get('event_ticker','').split('-')[0],{}) if is_k else {};title=raw.get('title','') if is_k else raw.get('question','');subtitle=raw.get('yes_sub_title',raw.get('subtitle','')) if is_k else raw.get('groupItemTitle','');event_title=event.get('title','');rules=(raw.get('rules_primary','')+'\n'+raw.get('rules_secondary','')) if is_k else raw.get('description','');text=norm(title+' '+subtitle+' '+event_title);rule_lower=rules.casefold();slug=raw.get('slug','');series_id=event.get('series_ticker') or raw.get('event_ticker','').split('-')[0];league=None
    if is_k:
        for code,v in sorted(LEAGUES.items(),key=lambda x:-len(x[0])):
            if series_id.startswith('KX'+code):league=v;break
    elif slug:
        prefix=slug.split('-')[0];league={'ncaaf':'cfb','ncaab':'ncaab','bundesliga':'bun','laliga':'lal','seriea':'sea','ligue1':'fl1'}.get(prefix,prefix) if prefix in set(LEAGUES.values())|{'ncaaf','bundesliga','laliga','seriea','ligue1'} else None
    if not league:
        for code,v in LEAGUES.items():
            if re.search(r'\b'+code.lower()+r'\b',text):league=v;break
    entities=[];predicate='CATEGORICAL_EVENT';reference=None
    for alias in ['btc','eth','solana','xrp','dogecoin','doge']:
        if re.search(r'\b'+alias+r'\b',text):entities.append({'doge':'dogecoin'}.get(alias,alias))
    if any(e in entities for e in ['btc','eth','solana','xrp','dogecoin']):predicate='ASSET_LEVEL';
    if re.search(r'\bup (?:or )?down\b',text):predicate='ASSET_RETURN'
    if 'temperature' in text or (is_k and re.match(r'KX(?:HIGH|LOW|RAIN|SNOW)',series_id)):
        predicate='WEATHER_LOW' if 'lowest' in text or 'low temperature' in text else 'WEATHER_HIGH';entities.append('temperature')
        for city in ['nyc','chicago','miami','los angeles','austin','dallas','seattle','boston','denver','philadelphia','san francisco','washington','london','seoul','paris','toronto']:
            if re.search(r'\b'+city+r'\b',text):entities.append('city:'+city)
    for expr,name in [(r'\bcpi\b|consumer price','US_CPI'),(r'\bgdp\b|gross domestic','US_GDP'),('unemployment','US_UNEMPLOYMENT'),('federal reserve|fed rate|fed funds|fomc','US_FED_FUNDS')]:
        if re.search(expr,text):entities.append(name);predicate=name+('_YOY' if 'year over year' in text or 'annual' in text else '_MOM' if name=='US_CPI' else '')
    if 'nomination' in text:predicate='POLITICAL_NOMINATION'
    elif re.search(r'\b(president|senate|governor|election|prime minister)\b',text):predicate='POLITICAL_ELECTION'
    if 'alien' in text or 'extraterrestrial' in text:entities.append('US_ALIEN_CONFIRMATION');predicate='OCCURRENCE_BEFORE_DEADLINE'
    if league:
        sm=str(raw.get('sportsMarketType','')).lower()
        if 'player_points' in sm or (is_k and 'PTS' in series_id):predicate='SPORTS_PLAYER_POINTS'
        elif 'player_rebounds' in sm or (is_k and 'REB' in series_id):predicate='SPORTS_REBOUNDS'
        elif 'player_assists' in sm or (is_k and 'AST' in series_id):predicate='SPORTS_ASSISTS'
        elif 'player_threes' in sm or (is_k and '3PT' in series_id):predicate='SPORTS_THREES'
        elif 'champion' in text or 'finals' in text or 'super bowl' in text:predicate='SPORTS_CHAMPION'
        elif re.search(r'\b(points|pts)\b',norm(title+' '+subtitle)):predicate='SPORTS_PLAYER_POINTS' if ':' in title or 'PLAYER' in series_id else 'SPORTS_TOTAL'
        elif 'spread' in text or 'wins by' in text or 'SPREAD' in series_id:predicate='SPORTS_SPREAD'
        elif 'total' in norm(title) or 'TOTAL' in series_id:predicate='SPORTS_TOTAL'
        elif 'first scorer' in text:predicate='SPORTS_FIRST_SCORER'
        elif 'rebound' in text or 'REBOUND' in series_id:predicate='SPORTS_REBOUNDS'
        elif 'assist' in text or 'ASSIST' in series_id:predicate='SPORTS_ASSISTS'
        elif '3 pointers' in text or 'three pointers' in text or '3PT' in series_id:predicate='SPORTS_THREES'
        elif 'touchdown' in text or 'TD' in series_id:predicate='SPORTS_TOUCHDOWN'
        elif 'strikeout' in text:predicate='SPORTS_STRIKEOUTS'
        else:predicate='SPORTS_GAME_WIN'
    if raw.get('mve_collection_ticker') or raw.get('mve_selected_legs'):predicate='COMBO'
    op=raw.get('strike_type') if is_k else None;threshold=raw.get('floor_strike') if is_k else None
    if threshold is None:
        m=re.search(r'\b(?:above|below|over|under|at least|at most|greater than|less than|exceed(?:s)?)\s*\$?(-?\d[\d,]*(?:\.\d+)?)([km])?',(title+' '+subtitle).lower())
        if m:
            try:threshold=str(Decimal(m[1].replace(',',''))*({'k':1000,'m':1000000}.get(m[2],1)))
            except Exception:pass
            op='less' if re.search(r'below|under|less',m[0]) else ('greater_or_equal' if 'at least' in m[0] else 'greater')
    threshold=str(Decimal(str(threshold)).normalize()) if threshold is not None else None
    for expr,index in [('cf benchmarks','CF_BENCHMARKS'),('chainlink','CHAINLINK'),('binance','BINANCE'),('national weather service','NWS'),('bureau of labor statistics','BLS')]:
        if expr in rule_lower:reference=index;break
    date,date_evidence=event_date(raw,venue);years=sorted(set(re.findall(r'\b20\d{2}\b',text)));period=None
    month=re.search(r'\b(january|february|march|april|may|june|july|august|september|october|november|december)\b',text)
    if month:period=month[1]+(' '+years[0] if len(years)==1 else '')
    version,outcomes,id_supported=('kalshi_fixed_point',[{'label':'Yes','asset_id':'YES','id_kind':'KALSHI_SIDE'},{'label':'No','asset_id':'NO','id_kind':'KALSHI_SIDE'}],True) if is_k else outcome_ids(raw,record['partition'])
    participant_names=participants(league,title+' '+event_title+' '+subtitle);yes_candidates=participants(league,subtitle) or participants(league,title);yes_entity=yes_candidates[0] if len(yes_candidates)==1 else None;subject=None
    if predicate.startswith('POLITICAL') or predicate.startswith('SPORTS_PLAYER') or predicate in ['SPORTS_FIRST_SCORER','SPORTS_REBOUNDS','SPORTS_ASSISTS','SPORTS_THREES','SPORTS_TOUCHDOWN']:
        match=re.search(r'^will (.+?) (?:win|be |become|get|receive|score|record|have)',norm(title));subject=match[1].strip() if match else (norm(title.split(':')[0]) if ':' in title else norm(subtitle) if subtitle and len(norm(subtitle).split())<=5 and norm(subtitle) not in ['yes','no'] else None)
        if subject:entities.append('subject:'+subject)
    labels=[x['label'].lower() for x in outcomes];category=event.get('category') or ser.get('category') or raw.get('category');category_source='OFFICIAL_EVENT_OR_SERIES' if category else 'DERIVED_FEATURES'
    if not category:category='sports' if league else 'crypto' if predicate.startswith('ASSET') else 'weather' if predicate.startswith('WEATHER') else 'economics' if predicate.startswith('US_') and predicate!='US_ALIEN_CONFIRMATION' else 'politics' if predicate.startswith('POLITICAL') else 'other'
    return {'venue':venue,'uid':('K:' if is_k else 'P:')+identifier,'market_id':identifier,'event_id':raw.get('event_ticker') if is_k else str(event.get('id')) if event.get('id') is not None else None,'event_title':event_title,'series_id':series_id if is_k else None,'condition_id':None if is_k else raw.get('conditionId'),'request_id':None if is_k else raw.get('requestId'),'outcome_ids':outcomes,'title':title,'subtitle':subtitle,'normalized_title':norm(title+' '+subtitle),'event_normalized_title':norm(event_title),'event_category':str(category).lower(),'category_source':category_source,'entities':entities,'event_predicate':predicate,'outcome_definition':subtitle or title,'numerical_threshold':threshold,'comparison_operator':op,'reference_index':reference,'observation_timestamp':raw.get('observation_timestamp') or raw.get('observationTimestamp'),'observation_window':None,'event_timezone':None,'sport_or_competition':league,'participant_names':participant_names,'yes_outcome_entity':yes_entity,'outcome_subject':subject,'event_date':date,'event_date_evidence':date_evidence,'years':years,'period':period,'resolution_source':event.get('settlement_sources') if is_k else raw.get('resolutionSource') or None,'source_rule_hash':hashlib.sha256(rules.encode()).hexdigest(),'rules':rules,'market_status':raw.get('status') if is_k else ('closed' if raw.get('closed') else 'active' if raw.get('active') else 'inactive'),'accepting_orders':True if is_k else raw.get('acceptingOrders'),'protocol_version':version,'reported_protocol_version':raw.get('version'),'outcome_id_mapping_supported':id_supported,'existing_ws_adapter_supported':is_k and predicate!='COMBO' or version=='v1' and labels in [['yes','no'],['no','yes'],['up','down'],['down','up']],'settlement_asset':('USD' if is_k else raw.get('collateralToken') or raw.get('collateralAddress')),'minimum_quantity':raw.get('minimum_order_size_fp') if is_k else raw.get('orderMinSize'),'quantity_step':raw.get('quantity_step_fp') if is_k else raw.get('quantityStep'),'price_tick':None if is_k else raw.get('orderPriceMinTickSize'),'price_ranges':raw.get('price_ranges') if is_k else None,'close_time':raw.get('close_time') if is_k else raw.get('endDate'),'expiration_time':raw.get('expiration_time') if is_k else None,'source':record['source'],'partition':record['partition'],'url':'https://kalshi.com/markets/'+identifier if is_k else 'https://polymarket.com/event/'+event.get('slug',slug or identifier),'slug':slug,'semantic_tokens':sorted(words(title+' '+subtitle+' '+event_title)),'sports_market_type':raw.get('sportsMarketType'),'neg_risk':raw.get('negRisk') if not is_k else None}

def match_features(k,p):
    conflicts=[];support=[]
    for f in ['numerical_threshold','event_date','observation_timestamp','period']:
        if k[f] is not None and p[f] is not None:
            if f=='event_date' and k[f]!=p[f] and abs((datetime.fromisoformat(k[f])-datetime.fromisoformat(p[f])).days)<=1:support.append('EVENT_DATE_TIMEZONE_AMBIGUITY')
            else:(support if k[f]==p[f] else conflicts).append(f+('_SAME' if k[f]==p[f] else '_DIFFERENT'))
    if k['years'] and p['years'] and not set(k['years'])&set(p['years']):conflicts.append('YEAR_OR_SEASON_DIFFERENT')
    if k['reference_index'] and p['reference_index'] and k['reference_index']!=p['reference_index']:conflicts.append('REFERENCE_INDEX_DIFFERENT')
    if k['sport_or_competition'] and p['sport_or_competition'] and k['sport_or_competition']!=p['sport_or_competition']:conflicts.append('LEAGUE_DIFFERENT')
    if k['event_predicate']!=p['event_predicate']:conflicts.append('PREDICATE_DIFFERENT')
    if k['entities'] and p['entities']:
        if set(k['entities'])&set(p['entities']):support.append('ENTITY_OVERLAP')
        elif any(x in ['btc','eth','US_ALIEN_CONFIRMATION'] for x in k['entities']+p['entities']):conflicts.append('UNDERLYING_ENTITY_DIFFERENT')
    if k.get('outcome_subject') and p.get('outcome_subject') and k['outcome_subject']!=p['outcome_subject']:
        if not (set(k['outcome_subject'].split())<=set(p['semantic_tokens']) or set(p['outcome_subject'].split())<=set(k['semantic_tokens'])):conflicts.append('OUTCOME_SUBJECT_DIFFERENT')
    if len(k['participant_names'])==2 and len(p['participant_names'])==2 and k['participant_names']!=p['participant_names']:conflicts.append('SPORTS_MATCHUP_DIFFERENT')
    if k['participant_names'] and p['participant_names'] and k['participant_names']==p['participant_names']:support.append('PARTICIPANT_SET_SAME')
    kc={e for e in k['entities'] if e.startswith('city:')};pc={e for e in p['entities'] if e.startswith('city:')}
    if kc and pc and not kc&pc:conflicts.append('CITY_DIFFERENT')
    a,b=set(k['semantic_tokens']),set(p['semantic_tokens']);j=len(a&b)/max(1,len(a|b));return support,conflicts,j

def retrieve(markets,run,semantic_neighbors=8):
    ps=[m for m in markets if m['venue']=='polymarket_international'];ks=[m for m in markets if m['venue']=='kalshi'];index=defaultdict(list);exact=defaultdict(list);events=defaultdict(list);numeric=defaultdict(list);sports=defaultdict(list);entity=defaultdict(list);combo_exact=defaultdict(list)
    for i,p in enumerate(ps):
        exact[p['normalized_title']].append(i)
        if p['event_normalized_title']:events[p['event_normalized_title']].append(i)
        for word in p['semantic_tokens']:index[word].append(i)
        for e in p['entities']:
            if p['numerical_threshold'] is not None:numeric[(e,p['numerical_threshold'])].append(i)
            entity[(e,p['event_predicate'],p['event_date'],() if p['event_date'] else tuple(p['years']))].append(i)
        if p['sport_or_competition'] and p['event_date']:sports[(p['sport_or_competition'],p['event_date'])].append(i)
        combo_exact[p['normalized_title']].append(i)
    semantic_cache={};pair_count=0;classifications=Counter();method_counts=Counter();max_pool=0;unretrieved=[];out=run/'candidate_matching_evidence.jsonl';stream=out.open('w');anchors=defaultdict(list);conditions={p.get('condition_id'):i for i,p in enumerate(ps) if p.get('condition_id')}
    for pair in json.loads((Path(__file__).resolve().parents[2]/'config/cross_venue_v0_1_2.json').read_text()).get('diagnostic_watch_pairs',[]):
        if pair['polymarket_market'] in conditions:anchors[pair['kalshi_market']].append(conditions[pair['polymarket_market']])
    for n,k in enumerate(ks):
        candidates=defaultdict(set);normtitle=k['normalized_title']
        if k['event_predicate']=='COMBO':
            for i in combo_exact.get(normtitle,[]):candidates[i].add('EXACT_COMPOSITE_TITLE_ONLY')
        else:
            for i in exact.get(normtitle,[]):candidates[i].add('EXACT_NORMALIZED_TITLE')
            if k['event_normalized_title']:
                for i in events.get(k['event_normalized_title'],[]):
                    if k['event_predicate']==ps[i]['event_predicate'] and (set(k['semantic_tokens'])&set(ps[i]['semantic_tokens'])):candidates[i].add('EVENT_LEVEL_THEN_OUTCOME_FEATURES')
            for e in k['entities']:
                for i in numeric.get((e,k['numerical_threshold']),[]) if k['numerical_threshold'] is not None else []:
                    if not k['event_date'] or not ps[i]['event_date'] or abs((datetime.fromisoformat(k['event_date'])-datetime.fromisoformat(ps[i]['event_date'])).days)<=1:candidates[i].add('ENTITY_NUMERIC_PREDICATE')
                key=(e,k['event_predicate'],k['event_date'],() if k['event_date'] else tuple(k['years']))
                for i in entity.get(key,[]):
                    if e.startswith('US_') or e.startswith('city:') or e.startswith('subject:') or e in ['btc','eth']:
                        if k['numerical_threshold']==ps[i]['numerical_threshold'] or k['numerical_threshold'] is None or ps[i]['numerical_threshold'] is None:candidates[i].add('STRUCTURED_ENTITY_EVENT_TYPE')
            if k['sport_or_competition'] and k['event_date']:
                for i in (i for day in [k['event_date'],(datetime.fromisoformat(k['event_date'])+__import__('datetime').timedelta(days=1)).date().isoformat(),(datetime.fromisoformat(k['event_date'])-__import__('datetime').timedelta(days=1)).date().isoformat()] for i in sports.get((k['sport_or_competition'],day),[])):
                    p=ps[i];common=set(k['semantic_tokens'])&set(p['semantic_tokens'])
                    if len(common)>=2 and k['event_predicate']==p['event_predicate']:candidates[i].add('SPORT_LEAGUE_DATE_PARTICIPANT_TOKENS')
            # Multiple independent text and structured methods; no economic cap.
            cache_key=tuple(k['semantic_tokens'])
            if cache_key in semantic_cache:best=semantic_cache[cache_key]
            else:
                scores=defaultdict(float);rare=sorted((w for w in k['semantic_tokens'] if w in index),key=lambda w:len(index[w]))[:8]
                for word in rare:
                    weight=math.log((1+len(ps))/(1+len(index[word])))+1
                    for i in index[word]:scores[i]+=weight
                max_pool=max(max_pool,len(scores));best=heapq.nlargest(semantic_neighbors,scores,key=lambda i:(scores[i],-i));semantic_cache[cache_key]=[(i,scores[i]) for i in best]
            if best and isinstance(best[0],int):best=[(i,scores[i]) for i in best]
            for i,score in best:
                support,conflicts,j=match_features(k,ps[i]);shared=len(set(k['semantic_tokens'])&set(ps[i]['semantic_tokens']))
                if shared>=2 and (j>=.30 or (shared>=3 and score>=9)):candidates[i].add('GLOBAL_WEIGHTED_LEXICAL_SEMANTIC_RETRIEVAL')
        for i in anchors.get(k['market_id'],[]):candidates[i].add('HISTORICAL_CANDIDATE_RETRIEVAL_NOT_APPROVAL')
        if not candidates and k['event_predicate']!='COMBO':unretrieved.append(k['uid'])
        for i,methods in candidates.items():
            p=ps[i];support,conflicts,j=match_features(k,p);severe=any(c in conflicts for c in ['UNDERLYING_ENTITY_DIFFERENT','LEAGUE_DIFFERENT','YEAR_OR_SEASON_DIFFERENT','event_date_DIFFERENT','OUTCOME_SUBJECT_DIFFERENT','SPORTS_MATCHUP_DIFFERENT','CITY_DIFFERENT']);related=any(c in conflicts for c in ['numerical_threshold_DIFFERENT','REFERENCE_INDEX_DIFFERENT','PREDICATE_DIFFERENT','observation_timestamp_DIFFERENT','period_DIFFERENT'])
            exact_match='EXACT_NORMALIZED_TITLE' in methods
            if severe:classification='CONTRADICTORY';confidence=.15
            elif related:classification='RELATED_BUT_NOT_IDENTICAL';confidence=.45
            elif exact_match or (j>=.70 and (support or len(set(k['semantic_tokens'])&set(p['semantic_tokens']))>=4)):classification='SAME_EVENT_HIGH_CONFIDENCE';confidence=.95 if exact_match else .88
            elif j>=.42 or len(support)>=2 or any(m in methods for m in ['ENTITY_NUMERIC_PREDICATE','STRUCTURED_ENTITY_EVENT_TYPE','EVENT_LEVEL_THEN_OUTCOME_FEATURES']):classification='SAME_EVENT_POSSIBLE';confidence=.65
            else:classification='UNRESOLVED';confidence=.35
            pair_id=hashlib.sha256((k['market_id']+'\0'+p['market_id']).encode()).hexdigest();pair={'pair_id':pair_id,'kalshi_uid':k['uid'],'polymarket_uid':p['uid'],'kalshi_market_id':k['market_id'],'polymarket_market_id':p['market_id'],'match_classification':classification,'match_confidence':confidence,'matching_methods':sorted(methods),'matching_features':support,'conflicting_features':conflicts,'title_token_jaccard':j,'event_category':k['event_category'],'source_rule_hashes':[k['source_rule_hash'],p['source_rule_hash']],'source_hashes':[k['source']['raw_sha256'],p['source']['raw_sha256']],'evidence_wall_ns':[k['source']['received_wall_ns'],p['source']['received_wall_ns']],'contractual_approval':False};method_counts.update(methods);classifications[classification]+=1;pair_count+=1;stream.write(json.dumps(pair,separators=(',',':'))+'\n')
        if n and n%25000==0:print(json.dumps({'markets_processed':n,'unique_pairs':pair_count,'stage':'MATCHING_NOT_ECONOMIC_SCREENING'}),flush=True)
    stream.close()
    write(run/'matching_manifest.json',{'kalshi_markets_considered':len(ks),'polymarket_markets_considered':len(ps),'unique_market_level_pairs':pair_count,'method_counts':method_counts,'classification_counts':classifications,'global_semantic_neighbors_per_market':semantic_neighbors,'semantic_rule':'Union with exact, event, entity/numeric, sports and structured retrieval; semantic top-neighbors alone do not exhaust pair space','max_semantic_posting_pool':max_pool,'ordinary_markets_without_retrieved_candidate':len(unretrieved),'no_pre_matching_liquidity_volume_period_category_or_price_filter':True,'combo_matching_status':'Exact composite title only; semantic leg-composition overlap incomplete and cannot be called zero','global_semantic_recall_proven':False,'candidate_evidence_sha256':hashlib.sha256(out.read_bytes()).hexdigest()});write(run/'unmatched_market_ids.json',unretrieved)
    print('matching complete',pair_count,flush=True)

def run(root):
    configure_teams(json.loads((root/'supporting/all_teams.json').read_text()));events={}
    for line in (root/'supporting/kalshi_events.jsonl').open():
        x=json.loads(line);events[x['raw']['event_ticker']]=x['raw']
    series={s['ticker']:s for s in json.loads((root/'supporting/kalshi_series.json').read_text())['series']};markets=[];counts=Counter();out=root/'canonical_market_universe.jsonl'
    with out.open('w') as f, (root/'normalization_inputs.jsonl').open('w') as native_input:
        for name in ['kalshi_all_open_markets.jsonl','polymarket_all_open_markets.jsonl']:
            for line in (root/name).open():
                record=json.loads(line);c=canonical(record,events,series);raw=record['raw'];fields=['ticker','event_ticker','title','rules_primary','rules_secondary','close_time','status','market_type','price_ranges','minimum_order_size_fp','quantity_step_fp','conditionId','question','description','endDate','version','outcomes','clobTokenIds','feeSchedule','feesEnabled','orderPriceMinTickSize','orderMinSize','quantityStep','active','closed','enableOrderBook','acceptingOrders','collateralToken','collateralAddress','negRisk'];native_raw={k:raw[k] for k in fields if k in raw};native_raw['events']=[{'id':e['id']} for e in raw.get('events',[]) if 'id' in e];native_input.write(json.dumps({'venue':record['venue'],'id':record['id'],'raw':native_raw,'source':{'received_wall_ns':record['source']['received_wall_ns']}},separators=(',',':'))+'\n');f.write(json.dumps(c,separators=(',',':'))+'\n');counts[(c['venue'],c['protocol_version'])]+=1
                view={key:c[key] for key in ['venue','uid','market_id','condition_id','period','normalized_title','event_normalized_title','event_category','entities','event_predicate','numerical_threshold','observation_timestamp','reference_index','event_date','years','sport_or_competition','participant_names','outcome_subject','semantic_tokens','source_rule_hash']};view['source']={key:c['source'][key] for key in ['raw_sha256','received_wall_ns']};markets.append(view)
    write(root/'canonical_manifest.json',{'counts':{'|'.join(k):v for k,v in counts.items()},'source_universe_hashes':{n:hashlib.sha256((root/n).read_bytes()).hexdigest() for n in ['kalshi_all_open_markets.jsonl','polymarket_all_open_markets.jsonl']},'all_input_records_retained':True,'native_normalizer_reuse':'Book and fee calculation uses native adapters; canonical inventory preserves V2 and non-YES/NO identifiers beyond the legacy WS adapter.'})
    retrieve(markets,root)
def rematch(root):
    configure_teams(json.loads((root/'supporting/all_teams.json').read_text()));path=root/'canonical_market_universe.jsonl';temp=root/'canonical_updated.jsonl';markets=[]
    with temp.open('w') as out:
        for line in path.open():
            c=json.loads(line)
            if c['event_predicate']!='COMBO':
                c['normalized_title']=norm(c['title']+' '+(c.get('subtitle') or ''));c['event_normalized_title']=norm(c.get('event_title') or '');c['semantic_tokens']=sorted(words(c['title']+' '+(c.get('subtitle') or '')+' '+(c.get('event_title') or '')))
                if c['venue']!='kalshi' and not c['event_date']:c['event_date'],c['event_date_evidence']=event_date({'question':c['title'],'slug':c.get('slug') or ''},c['venue'])
                if c['event_predicate'].startswith('WEATHER') and 'nyc' in c['normalized_title'] and 'city:nyc' not in c['entities']:c['entities'].append('city:nyc')
                c['participant_names']=participants(c['sport_or_competition'],c['title']+' '+(c.get('event_title') or '')+' '+(c.get('subtitle') or ''))
                possible=participants(c['sport_or_competition'],c.get('subtitle') or '') or participants(c['sport_or_competition'],c['title']);c['yes_outcome_entity']=possible[0] if len(possible)==1 else None
            out.write(json.dumps(c,separators=(',',':'))+'\n');view={key:c[key] for key in ['venue','uid','market_id','condition_id','period','normalized_title','event_normalized_title','event_category','entities','event_predicate','numerical_threshold','observation_timestamp','reference_index','event_date','years','sport_or_competition','participant_names','outcome_subject','semantic_tokens','source_rule_hash']};view['source']={key:c['source'][key] for key in ['raw_sha256','received_wall_ns']};markets.append(view)
    intermediate=root/'intermediate';intermediate.mkdir(exist_ok=True)
    if not (intermediate/'canonical_initial.jsonl').exists():path.rename(intermediate/'canonical_initial.jsonl')
    else:path.unlink()
    temp.rename(path);manifest=json.loads((root/'canonical_manifest.json').read_text());manifest['matching_feature_revision']=2;manifest['canonical_sha256']=hashlib.sha256(path.read_bytes()).hexdigest();write(root/'canonical_manifest.json',manifest);retrieve(markets,root)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--run-dir',type=Path,required=True);p.add_argument('--rematch',action='store_true');a=p.parse_args();(rematch if a.rematch else run)(a.run_dir.resolve())
