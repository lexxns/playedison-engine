-- proc_fusion_spell.lua
--
-- Upstream ProjectIgnis/CardScripts puts the generic Fusion-Summon effect
-- (`Fusion.CreateSummonEff`) and its machinery here.  This fork had blanked the
-- file, so `Fusion.CreateSummonEff` was nil and the two pool cards that use it
-- died while registering their effect:
--
--     [lua t=0] [string "c80033124.lua"]:6: attempt to call a nil value
--               (field 'CreateSummonEff')
--
-- Ported from upstream, minus the parts of that file that are not about Fusion
-- Spells (the global "is able to X" / reason / ReloadField workarounds), which
-- this fork keeps out on purpose — see docs/card-scripts.md.  Everything here
-- delegates to bindings this era core already has:
--   Duel.GetFusionMaterial, Duel.SelectFusionMaterial, Card.CheckFusionMaterial,
--   Card.IsCanBeFusionMaterial, Duel.GetChainMaterial, Group.Split/Select/...
-- Two names the upstream file relies on are missing here and are supplied below:
--   Group.AddMaximumCheck (a Rush "maximum" marker; a no-op in this pool) and
--   aux.GrouptoCardid.

Fusion.ExtraGroup=nil

-- Rush "maximum" bookkeeping does not exist in this core; the upstream calls
-- only ever use the returned group, so identity is the correct era shim.
if not Group.AddMaximumCheck then
	function Group.AddMaximumCheck(g) return g end
end

function aux.GrouptoCardid(g)
	local res={}
	for card in aux.Next(g) do
		res[card:GetCardID()] = true
	end
	return res
end

-- Upstream registers this global effect at load time.  This fork segfaults if
-- `Duel.RegisterEffect` runs while the scripts are being loaded (during
-- `OCG_DuelNewCard`, before `OCG_StartDuel`), so register it lazily on the first
-- in-duel use instead.  The effect only ever marks `Fusion.ExtraGroup` as
-- available; the extrafil paths also merge that group explicitly, so a card
-- whose effect never opens a Fusion Summon never needs it.
local geff=nil
local function EnsureGeff()
	if geff then return geff end
	geff=Effect.GlobalEffect()
	geff:SetType(EFFECT_TYPE_FIELD)
	geff:SetCode(EFFECT_EXTRA_FUSION_MATERIAL)
	geff:SetProperty(EFFECT_FLAG_SET_AVAILABLE+EFFECT_FLAG_IGNORE_IMMUNE)
	geff:SetTargetRange(0xff,0xff)
	geff:SetTarget(function(e,c)
		return Fusion.ExtraGroup and Fusion.ExtraGroup:IsContains(c)
	end)
	geff:SetValue(aux.TRUE)
	Duel.RegisterEffect(geff,0)
	return geff
end

Duel.GetFusionMaterial=(function()
	local oldfunc=Duel.GetFusionMaterial
	return function(tp)
		EnsureGeff()
		local res=oldfunc(tp)
		local g=Duel.GetMatchingGroup(Card.IsHasEffect,tp,LOCATION_EXTRA,0,nil,EFFECT_EXTRA_FUSION_MATERIAL)
		if #g>0 then
			res:Merge(g)
		end
		return res
	end
end)()

--Returns the first EFFECT_EXTRA_FUSION_MATERIAL applied on Card c.
--If summon_card is provided, it will also check if the effect's value function applies to that card.
--Card.IsHasEffect alone cannot be used because it would return the above effect as well.
local function GetExtraMatEff(c,summon_card)
	local effs={c:IsHasEffect(EFFECT_EXTRA_FUSION_MATERIAL)}
	for _,eff in ipairs(effs) do
		if eff~=geff then
			if not summon_card then
				return eff
			end
			local val=eff:GetValue()
			if (type(val)=="function" and val(eff,summon_card)) or val==1 then
				return eff
			end
		end
	end
end
--Once per turn check for EFFECT_EXTRA_FUSION_MATERIAL effects.
--Removes cards from the material pool group if the OPT of the
--EFFECT_EXTRA_FUSION_MATERIAL effect has already been used.
local function ExtraMatOPTCheck(mg1,e,tp,extrafil,efmg)
	local extra_feff_mg=mg1:Filter(GetExtraMatEff,nil)
	if #extra_feff_mg>0 then
		local extra_feff=GetExtraMatEff(extra_feff_mg:GetFirst())
		--Check if you need to remove materials from the pool if count limit has been used
		if extra_feff and not extra_feff:CheckCountLimit(tp) then
			local extra_feff_loc=extra_feff:GetTargetRange()
			if extrafil then
				local extrafil_g=extrafil(e,tp,mg1)
				if extrafil_g and #extrafil_g>0 and not extrafil_g:IsExists(Card.IsLocation,1,nil,extra_feff_loc) then
					mg1:Sub(extra_feff_mg:Filter(Card.IsLocation,nil,extra_feff_loc))
					efmg:Clear()
				elseif not extrafil_g then
					mg1:Sub(extra_feff_mg:Filter(Card.IsLocation,nil,extra_feff_loc))
					efmg:Clear()
				end
			else
				mg1:Sub(extra_feff_mg:Filter(Card.IsLocation,nil,extra_feff_loc))
				efmg:Clear()
			end
		end
	elseif #efmg>0 then
		local extra_feff=GetExtraMatEff(efmg:GetFirst())
		if extra_feff and not extra_feff:CheckCountLimit(tp) then
			efmg:Clear()
		end
	end
	return mg1,efmg
end

Fusion.CreateSummonEff = aux.FunctionWithNamedArgs(
function(c,fusfilter,matfilter,extrafil,extraop,gc,stage2,exactcount,value,location,chkf,desc,preselect,nosummoncheck,extratg,mincount,maxcount,sumpos)
	local e1=Effect.CreateEffect(c)
	if desc then
		e1:SetDescription(desc)
	else
		e1:SetDescription(1170)
	end
	e1:SetCategory(CATEGORY_SPECIAL_SUMMON+CATEGORY_FUSION_SUMMON)
	e1:SetType(EFFECT_TYPE_ACTIVATE)
	e1:SetCode(EVENT_FREE_CHAIN)
	e1:SetTarget(Fusion.SummonEffTG(fusfilter,matfilter,extrafil,extraop,gc,stage2,exactcount,value,location,chkf,preselect,nosummoncheck,extratg,mincount,maxcount,sumpos))
	e1:SetOperation(Fusion.SummonEffOP(fusfilter,matfilter,extrafil,extraop,gc,stage2,exactcount,value,location,chkf,preselect,nosummoncheck,mincount,maxcount,sumpos))
	return e1
end,"handler","fusfilter","matfilter","extrafil","extraop","gc","stage2","exactcount","value","location","chkf","desc","preselect","nosummoncheck","extratg","mincount","maxcount","sumpos")
function Fusion.RegisterSummonEff(c,...)
	local tab=type(c)=="table"
	local e1=Fusion.CreateSummonEff(tab and c or c,...)
	Card.RegisterEffect((tab and c["handler"] or c),e1)
	return e1
end
function Fusion.SummonEffFilter(c,fusfilter,e,tp,mg,gc,chkf,value,sumlimit,nosummoncheck,sumpos,efmg)
	if efmg then
		mg:Merge(efmg:Filter(GetExtraMatEff,nil,c))
	end
	return c:IsType(TYPE_FUSION) and (not fusfilter or fusfilter(c,tp)) and (nosummoncheck or c:IsCanBeSpecialSummoned(e,value,tp,sumlimit,false,sumpos))
			and c:CheckFusionMaterial(mg,gc,chkf)
end

function Fusion.ForcedMatValidity(c,e)
	if c==e:GetHandler() then
		return not c:IsRelateToEffect(e)
	end
	return c:IsImmuneToEffect(e)
end

Fusion.SummonEffTG = aux.FunctionWithNamedArgs(
function(fusfilter,matfilter,extrafil,extraop,gc2,stage2,exactcount,value,location,chkf,preselect,nosummoncheck,extratg,mincount,maxcount,sumpos)
	sumpos = sumpos or POS_FACEUP
	return	function(e,tp,eg,ep,ev,re,r,rp,chk)
				location=location or LOCATION_EXTRA
				if not chkf or ((chkf&PLAYER_NONE)~=PLAYER_NONE) then
					chkf=chkf and chkf|tp or tp
				end
				local sumlimit=(chkf&(FUSPROC_NOTFUSION|FUSPROC_NOLIMIT))~=0
				local notfusion=(chkf&FUSPROC_NOTFUSION)~=0
				if not value then value=0 end
				value = value|MATERIAL_FUSION
				if not notfusion then
					value = value|SUMMON_TYPE_FUSION
				end
				local gc=gc2
				gc=type(gc)=="function" and gc(e,tp,eg,ep,ev,re,r,rp,chk) or gc
				gc=type(gc)=="Card" and Group.FromCards(gc) or gc
				matfilter=matfilter or Card.IsAbleToGrave
				stage2 = stage2 or aux.TRUE
				if chk==0 then
					local fmg_all=Duel.GetFusionMaterial(tp)
					local mg1=fmg_all:Filter(matfilter,nil,e,tp,0)
					local efmg=fmg_all:Filter(GetExtraMatEff,nil)
					local checkAddition=nil
					local repl_flag=false
					if #efmg>0 then
						local extra_feff=GetExtraMatEff(efmg:GetFirst())
						if extra_feff and extra_feff:GetLabelObject() then
							local repl_function=extra_feff:GetLabelObject()
							repl_flag=true
							if not extrafil then
								local ret = {repl_function[1](e,tp,mg1)}
								if ret[1] then
									ret[1]:Match(matfilter,nil,e,tp,0)
									if repl_function[2] then
										ret[1]:Match(repl_function[2],nil,e,tp)
										efmg:Match(repl_function[2],nil,e,tp)
									end
									Fusion.ExtraGroup=ret[1]:Filter(Card.IsCanBeFusionMaterial,nil,nil,value):Match(aux.NOT(Card.IsImmuneToEffect),nil,e)
									mg1:Merge(ret[1])
								end
								checkAddition=ret[2]
							elseif extrafil then
								local ret = {extrafil(e,tp,mg1)}
								local repl={repl_function[1](e,tp,mg1)}
								if ret[1] then
									repl[1]:Match(matfilter,nil,e,tp,0)
									ret[1]:Merge(repl[1])
									Fusion.ExtraGroup=ret[1]:Filter(Card.IsCanBeFusionMaterial,nil,nil,value):Match(aux.NOT(Card.IsImmuneToEffect),nil,e)
									mg1:Merge(ret[1])
								end
								if ret[2] then
									checkAddition=aux.AND(ret[2],repl[2])
								else
									checkAddition=repl[2]
								end
							end
						end
					end
					if not repl_flag and extrafil then
						local ret = {extrafil(e,tp,mg1)}
						if ret[1] then
							Fusion.ExtraGroup=ret[1]:Filter(Card.IsCanBeFusionMaterial,nil,nil,value):Match(aux.NOT(Card.IsImmuneToEffect),nil,e)
							mg1:Merge(ret[1])
						end
						checkAddition=ret[2]
					end
					if gc and not mg1:Includes(gc) then
						Fusion.ExtraGroup=nil
						return false
					end
					Fusion.CheckAdditional=checkAddition
					mg1:Match(Card.IsCanBeFusionMaterial,nil,nil,value):Match(aux.NOT(Card.IsImmuneToEffect),nil,e)
					Fusion.CheckExact=exactcount
					Fusion.CheckMin=mincount
					Fusion.CheckMax=maxcount
					mg1,efmg=ExtraMatOPTCheck(mg1,e,tp,extrafil,efmg)
					local res=Duel.IsExistingMatchingCard(Fusion.SummonEffFilter,tp,location,0,1,nil,fusfilter,e,tp,mg1,gc,chkf,value&0xffffffff,sumlimit,nosummoncheck,sumpos,efmg)
					Fusion.CheckAdditional=nil
					Fusion.ExtraGroup=nil
					if not res and not notfusion then
						for _,ce in ipairs({Duel.GetPlayerEffect(tp,EFFECT_CHAIN_MATERIAL)}) do
							local fgroup=ce:GetTarget()
							local mg=fgroup(ce,e,tp,value)
							if #mg>0 and (not Fusion.CheckExact or #mg==Fusion.CheckExact) and (not Fusion.CheckMin or #mg>=Fusion.CheckMin) then
								local mf=ce:GetValue()
								local fcheck=nil
								if ce:GetLabelObject() then fcheck=ce:GetLabelObject():GetOperation() end
								Fusion.CheckAdditional=checkAddition
								if fcheck then
									if checkAddition then Fusion.CheckAdditional=aux.AND(checkAddition,fcheck) else Fusion.CheckAdditional=fcheck end
								end
								Fusion.ExtraGroup=mg
								if Duel.IsExistingMatchingCard(Fusion.SummonEffFilter,tp,location,0,1,nil,aux.AND(mf,fusfilter or aux.TRUE),e,tp,mg,gc,chkf,value,sumlimit,nosummoncheck,sumpos) then
									res=true
									Fusion.CheckAdditional=nil
									Fusion.ExtraGroup=nil
									break
								end
								Fusion.CheckAdditional=nil
								Fusion.ExtraGroup=nil
							end
						end
					end
					Fusion.CheckExact=nil
					Fusion.CheckMin=nil
					Fusion.CheckMax=nil
					return res
				end
				Duel.SetOperationInfo(0,CATEGORY_SPECIAL_SUMMON,nil,1,tp,location)
				if extratg then extratg(e,tp,eg,ep,ev,re,r,rp,chk) end
			end
end,"fusfilter","matfilter","extrafil","extraop","gc","stage2","exactcount","value","location","chkf","preselect","nosummoncheck","extratg","mincount","maxcount","sumpos")
function Fusion.ChainMaterialPrompt(effswithgroup,cardID,tp,e)
	local effs = {}
	for i,eff in ipairs(effswithgroup) do
		if eff[2][cardID] ~= nil then
			table.insert(effs,i)
		end
	end
	if #effs == 1 then return effs[1] end
	local desctable = {}
	for _,index in ipairs(effs) do
		table.insert(desctable,effswithgroup[index][1]:GetDescription())
	end
	return effs[Duel.SelectOption(tp,false,table.unpack(desctable)) + 1]
end
Fusion.SummonEffOP = aux.FunctionWithNamedArgs(
function (fusfilter,matfilter,extrafil,extraop,gc2,stage2,exactcount,value,location,chkf,preselect,nosummoncheck,mincount,maxcount,sumpos)
	sumpos = sumpos or POS_FACEUP
	return	function(e,tp,eg,ep,ev,re,r,rp)
				location=location or LOCATION_EXTRA
				if not chkf or ((chkf&PLAYER_NONE)~=PLAYER_NONE) then
					chkf=chkf and chkf|tp or tp
				end
				if not preselect then chkf=chkf|FUSPROC_CANCELABLE end
				local sumlimit=(chkf&(FUSPROC_NOTFUSION|FUSPROC_NOLIMIT))~=0
				local notfusion=(chkf&FUSPROC_NOTFUSION)~=0
				if not value then value=0 end
				if not notfusion then
					value = value|SUMMON_TYPE_FUSION|MATERIAL_FUSION
				end
				local gc=gc2
				gc=type(gc)=="function" and gc(e,tp,eg,ep,ev,re,r,rp,chk) or gc
				gc=type(gc)=="Card" and Group.FromCards(gc) or gc
				matfilter=matfilter or Card.IsAbleToGrave
				stage2 = stage2 or aux.TRUE
				local checkAddition
				local fmg_all=Duel.GetFusionMaterial(tp)
				local mg1=fmg_all:Filter(matfilter,nil,e,tp,1)
				local efmg=fmg_all:Filter(GetExtraMatEff,nil)
				local extragroup=nil
				local repl_flag=false
				local function filter_material_immunity_and_necrovalley(card,fvalue,feffect)
					return card:IsCanBeFusionMaterial(nil,fvalue) and not card:IsImmuneToEffect(feffect) and aux.nvfilter(card)
				end
				if #efmg>0 then
					local extra_feff=GetExtraMatEff(efmg:GetFirst())
					if extra_feff and extra_feff:GetLabelObject() then
						local repl_function=extra_feff:GetLabelObject()
						repl_flag=true
						if not extrafil then
							local ret = {repl_function[1](e,tp,mg1)}
							if ret[1] then
								ret[1]:Match(matfilter,nil,e,tp,1)
								if repl_function[2] then
									ret[1]:Match(repl_function[2],nil,e,tp)
									efmg:Match(repl_function[2],nil,e,tp)
								end
								Fusion.ExtraGroup=ret[1]:Filter(filter_material_immunity_and_necrovalley,nil,value,e)
								mg1:Merge(ret[1])
							end
							checkAddition=ret[2]
						elseif extrafil then
							local ret = {extrafil(e,tp,mg1)}
							local repl={repl_function[1](e,tp,mg1)}
							if ret[1] then
								repl[1]:Match(matfilter,nil,e,tp,1)
								ret[1]:Merge(repl[1])
								Fusion.ExtraGroup=ret[1]:Filter(filter_material_immunity_and_necrovalley,nil,value,e)
								mg1:Merge(ret[1])
							end
							if ret[2] then
								checkAddition=aux.AND(ret[2],repl[2])
							else
								checkAddition=repl[2]
							end
						end
					end
				end
				if not repl_flag and extrafil then
					local ret = {extrafil(e,tp,mg1)}
					if ret[1] then
						Fusion.ExtraGroup=ret[1]:Filter(filter_material_immunity_and_necrovalley,nil,value,e)
						extragroup=ret[1]
						mg1:Merge(ret[1])
					end
					checkAddition=ret[2]
				end
				mg1:Match(filter_material_immunity_and_necrovalley,nil,value,e)
				if gc and (not mg1:Includes(gc) or gc:IsExists(Fusion.ForcedMatValidity,1,nil,e)) then
					Fusion.ExtraGroup=nil
					return false
				end
				Fusion.CheckExact=exactcount
				Fusion.CheckMin=mincount
				Fusion.CheckMax=maxcount
				Fusion.CheckAdditional=checkAddition
				local effswithgroup={}
				mg1,efmg=ExtraMatOPTCheck(mg1,e,tp,extrafil,efmg)
				local sg1=Duel.GetMatchingGroup(Fusion.SummonEffFilter,tp,location,0,nil,fusfilter,e,tp,mg1,gc,chkf,value&0xffffffff,sumlimit,nosummoncheck,sumpos,efmg)
				if #sg1>0 then
					table.insert(effswithgroup,{e,aux.GrouptoCardid(sg1)})
				end
				Fusion.ExtraGroup=nil
				Fusion.CheckAdditional=nil
				if not notfusion then
					local extraeffs = {Duel.GetPlayerEffect(tp,EFFECT_CHAIN_MATERIAL)}
					for _,ce in ipairs(extraeffs) do
						local fgroup=ce:GetTarget()
						local mg2=fgroup(ce,e,tp,value)
						if #mg2>0 and (not Fusion.CheckExact or #mg2==Fusion.CheckExact) and (not Fusion.CheckMin or #mg2>=Fusion.CheckMin) then
							local mf=ce:GetValue()
							local fcheck=nil
							if ce:GetLabelObject() then fcheck=ce:GetLabelObject():GetOperation() end
							Fusion.CheckAdditional=checkAddition
							if fcheck then
								if checkAddition then Fusion.CheckAdditional=aux.AND(checkAddition,fcheck) else Fusion.CheckAdditional=fcheck end
							end
							Fusion.ExtraGroup=mg2
							local sg2=Duel.GetMatchingGroup(Fusion.SummonEffFilter,tp,location,0,nil,aux.AND(mf,fusfilter or aux.TRUE),e,tp,mg2,gc,chkf,value,sumlimit,nosummoncheck,sumpos)
							if #sg2 > 0 then
								table.insert(effswithgroup,{ce,aux.GrouptoCardid(sg2)})
								sg1:Merge(sg2)
							end
							Fusion.CheckAdditional=nil
							Fusion.ExtraGroup=nil
						end
					end
				end
				if #sg1>0 then
					local sg=sg1:Clone()
					local mat1=Group.CreateGroup()
					local sel=nil
					local backupmat=nil
					local tc=nil
					local ce=nil
					while #mat1==0 do
						Duel.Hint(HINT_SELECTMSG,tp,HINTMSG_SPSUMMON)
						tc=sg:Select(tp,1,1,nil):GetFirst()
						if preselect and preselect(e,tc)==false then
							return
						end
						sel=effswithgroup[Fusion.ChainMaterialPrompt(effswithgroup,tc:GetCardID(),tp,e)]
						if sel[1]==e then
							Fusion.CheckAdditional=checkAddition
							Fusion.ExtraGroup=extragroup
							mat1=Duel.SelectFusionMaterial(tp,tc,mg1,gc,chkf)
						else
							ce=sel[1]
							local fcheck=nil
							if ce:GetLabelObject() then fcheck=ce:GetLabelObject():GetOperation() end
							Fusion.CheckAdditional=checkAddition
							if fcheck then
								if checkAddition then Fusion.CheckAdditional=aux.AND(checkAddition,fcheck) else Fusion.CheckAdditional=fcheck end
							end
							Fusion.ExtraGroup=ce:GetTarget()(ce,e,tp,value)
							mat1=Duel.SelectFusionMaterial(tp,tc,Fusion.ExtraGroup,gc,chkf)
						end
					end
					if sel[1]==e then
						Fusion.ExtraGroup=nil
						backupmat=mat1:Clone()
						if not notfusion then
							tc:SetMaterial(mat1)
						end
						local extra_feff_mg=mat1:Filter(GetExtraMatEff,nil,tc)
						if #extra_feff_mg>0 and extraop then
							local extra_feff=GetExtraMatEff(extra_feff_mg:GetFirst(),tc)
							if extra_feff then
								local extra_feff_op=extra_feff:GetOperation()
								if extra_feff_op and extraop~=extra_feff_op and extra_feff:CheckCountLimit(tp) then
									local flag=nil
									if extrafil then
										local extrafil_g=extrafil(e,tp,mg1)
										if #extrafil_g>=0 and not extrafil_g:IsExists(Card.IsLocation,1,nil,extra_feff:GetTargetRange()) then
											mat1:Sub(extra_feff_mg)
											extra_feff_op(e,tc,tp,extra_feff_mg)
											flag=true
										elseif #extrafil_g>=0 and Duel.SelectEffectYesNo(tp,extra_feff:GetHandler()) then
											Duel.Hint(HINT_SELECTMSG,tp,HINTMSG_RESOLVECARD)
											local g=extra_feff_mg:Select(tp,1,#extra_feff_mg,nil)
											if #g>0 then
												mat1:Sub(g)
												extra_feff_op(e,tc,tp,g)
												flag=true
											end
										end
									else
										mat1:Sub(extra_feff_mg)
										extra_feff_op(e,tc,tp,extra_feff_mg)
										flag=true
									end
									if flag and extra_feff:CheckCountLimit(tp) then
										extra_feff:UseCountLimit(tp,1)
									end
								end
							end
						end
						if extraop then
							if extraop(e,tc,tp,mat1)==false then return end
						end
						if #mat1>0 then
							local extra_feff_mg,normal_mg=mat1:Split(GetExtraMatEff,nil,tc)
							local extra_feff
							if #extra_feff_mg>0 then extra_feff=GetExtraMatEff(extra_feff_mg:GetFirst(),tc) end
							if #normal_mg>0 and (not extra_feff or extra_feff:GetLabel()~=160018042) then
								normal_mg=normal_mg:AddMaximumCheck()
								Duel.SendtoGrave(normal_mg,REASON_EFFECT+REASON_MATERIAL+REASON_FUSION)
							end
							if extra_feff then
								local extra_feff_op=extra_feff:GetOperation()
								if extra_feff_op then
									extra_feff_op(e,tc,tp,extra_feff_mg)
								else
									extra_feff_mg=extra_feff_mg:AddMaximumCheck()
									Duel.SendtoGrave(extra_feff_mg,REASON_EFFECT+REASON_MATERIAL+REASON_FUSION)
								end
								if extra_feff:CheckCountLimit(tp) then
									extra_feff:UseCountLimit(tp,1)
								end
							end
						end
						Duel.BreakEffect()
						Duel.SpecialSummonStep(tc,value,tp,tp,sumlimit,false,sumpos)
					else
						Fusion.CheckAdditional=nil
						Fusion.ExtraGroup=nil
						ce:GetOperation()(sel[1],e,tp,tc,mat1,value,nil,sumpos)
						backupmat=tc:GetMaterial():Clone()
					end
					stage2(e,tc,tp,backupmat,0)
					Duel.SpecialSummonComplete()
					stage2(e,tc,tp,backupmat,3)
					if (chkf&FUSPROC_NOTFUSION)==0 then
						tc:CompleteProcedure()
					end
					stage2(e,tc,tp,backupmat,1)
				end
				stage2(e,nil,tp,nil,2)
				Fusion.CheckMin=nil
				Fusion.CheckMax=nil
				Fusion.CheckExact=nil
				Fusion.CheckAdditional=nil
			end
end,"fusfilter","matfilter","extrafil","extraop","gc","stage2","exactcount","value","location","chkf","preselect","nosummoncheck","mincount","maxcount","sumpos")
function Fusion.BanishMaterial(e,tc,tp,sg)
	Duel.Remove(sg,POS_FACEUP,REASON_EFFECT+REASON_MATERIAL+REASON_FUSION)
	sg:Clear()
end
function Fusion.ShuffleMaterial(e,tc,tp,sg)
	local rg=sg:Filter(Card.IsFacedown,nil)
	local hg=sg:Filter(Card.IsLocation,nil,LOCATION_GRAVE|LOCATION_REMOVED)
	if #rg>0 then Duel.ConfirmCards(1-tp,rg) end
	if #hg>0 then Duel.HintSelection(hg,true) end
	local tg=sg:AddMaximumCheck()
	Duel.SendtoDeck(tg,nil,2,REASON_EFFECT|REASON_MATERIAL|REASON_FUSION)
	sg:Clear()
end
function Fusion.OnFieldMat(filter,...)
	if type(filter) == "Card" then
		return filter:IsOnField() and filter:IsAbleToGrave()
	end
	local funs={filter,...}
	return function (c,...)
		local res = c:IsOnField()
		if res then
			for i,fil in ipairs(funs) do
				if not fil(c,...) then
					return false
				end
			end
		end
		return res
	end
end
function Fusion.InHandMat(filter,...)
	if type(filter) == "Card" then
		return filter:IsLocation(LOCATION_HAND) and filter:IsAbleToGrave()
	end
	local funs={filter,...}
	return function (c,...)
		local res = c:IsLocation(LOCATION_HAND)
		if res then
			for i,fil in ipairs(funs) do
				if not fil(c,...) then
					return false
				end
			end
		end
		return res
	end
end
function Fusion.IsMonsterFilter(f1,...)
	local funs={f1,...}
	return	function(c,...)
		local res = c:IsMonster()
		if res then
			for i,fil in ipairs(funs) do
				if not fil(c,...) then
					return false
				end
			end
		end
		return res
	end
end
function Fusion.ForcedHandler(e)
	return e:GetHandler()
end
function Fusion.CheckWithHandler(fun,...)
	local funs={fun,...}
	return function(c,e,...)
		if c==e:GetHandler() then
			return true
		end
		for i,fil in ipairs(funs) do
			if fil(c,e,...) then
				return true
			end
		end
		return false
	end
end
