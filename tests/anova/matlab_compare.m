function matlab_compare(manifest_path)
% 使用MATLAB原生anovan拟合全模型和合并模型；Trial不是分析因子。
manifest=jsondecode(fileread(manifest_path));
for index=1:numel(manifest.cases)
    item=manifest.cases(index);
    t=readtable(item.csv,'TextType','string');
    [pv,full]=anovan(t.Measurement,{t.Part,t.Operator},'model','interaction','random',[1 2],'sstype',3,'varnames',{'Part','Operator'},'display','off');
    [~,pooled]=anovan(t.Measurement,{t.Part,t.Operator},'model','linear','random',[1 2],'sstype',3,'varnames',{'Part','Operator'},'display','off');
    result=struct('version',version,'records',height(t),'full',rows(full),'pooled',rows(pooled),'interaction_p',pv(3));
    result.imported=[t.StdOrder,double(char(t.Operator))-double('A')+1,t.Part,t.Trial,t.Measurement];
    file=fopen(fullfile(item.folder,'matlab.json'),'w');
    fprintf(file,'%s',jsonencode(result));fclose(file);
end
fprintf('MATLAB: %d cases complete\n',numel(manifest.cases));
end
function data=rows(table)
data=[];
for i=2:size(table,1)-1
    data=[data;[table{i,2},table{i,3},table{i,2}/table{i,3}]]; %#ok<AGROW>
end
end
